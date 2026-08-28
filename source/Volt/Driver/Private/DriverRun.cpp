// DriverRun.cpp — Driver::Run(): `volt run`, through BackendJIT.
//
// The mirror of DriverBuild.cpp, and deliberately shaped like it: the same
// three-phase protocol over the same UnitViews, differing only in which backend
// the views are handed to. That symmetry is the visible payoff of splitting the
// emission layer out — nothing about *emitting* Volt appears twice.
//
// Everything backend-specific stays inside this TU's VOLT_ENABLE_JIT branch;
// Driver.hpp mentions no backend type, so a caller (RunCommand.cpp) never needs
// an LLVM header either.

#include "Volt/Driver/Driver.hpp"

#ifdef VOLT_ENABLE_JIT
    #include "Volt/BackendCore/TargetBackend.hpp"
    #include "Volt/BackendJIT/JitBackend.hpp"

    #include "Volt/Core/Log/Logger.hpp"

    #include <algorithm>
    #include <atomic>
    #include <chrono>
    #include <cstdint>
    #include <filesystem>
    #include <iostream>
    #include <map>
    #include <span>
    #include <thread>
    #include <utility>
    #include <vector>
#endif

#include <optional>
#include <string>
#include <string_view>

#ifdef VOLT_ENABLE_JIT
namespace
{

// 200 ms: fast enough that a save feels immediate, slow enough that a watch on
// a large circuit costs nothing measurable. No inotify — one poll of a handful
// of stat() calls is portable, and portability wins here.
constexpr int PollInterval = 200;

// What "this file moved" is decided on. Modification time alone is not enough:
// an editor that writes through a temporary and renames can land on the same
// timestamp, and a truncating write can leave it untouched.
struct Stamp
{

    std::filesystem::file_time_type Written{};
    std::uintmax_t Size = 0;

    [[nodiscard]] bool operator==( const Stamp & ) const = default;
};

Stamp StampOf ( const std::string &Path )
{
    std::error_code Ec;
    const std::uintmax_t Size                     = std::filesystem::file_size( Path, Ec );
    const std::filesystem::file_time_type Written = std::filesystem::last_write_time( Path, Ec );
    return Ec ? Stamp{} : Stamp{ .Written = Written, .Size = Size };
}

// The program, on a thread of its own.
//
// Not an optimisation: it is what makes a *live* reload mean anything. With the
// program on this thread, `volt run --watch` could only poll once the program
// had returned, so a server or an event loop — the two programs a hot reload is
// actually for — never reached the watch loop at all. The one that finishes
// keeps behaving exactly as it did: it is reaped, and the next accepted reload
// starts it again.
//
// Nothing here is shared with the watch loop except the two atomics. The
// backend is: the watch loop compiles into it and patches slots while this
// thread executes the code those slots reach, which is the one interleaving the
// whole indirection was built for (jit.md, "Hot reload").
class ProgramThread
{

public:

    ProgramThread ( const ProgramThread & )           = delete;
    ProgramThread &operator=( const ProgramThread & ) = delete;
    ProgramThread ( ProgramThread && )                = delete;
    ProgramThread &operator=( ProgramThread && )      = delete;

    ProgramThread () = default;

    ~ProgramThread ()
    {
        // A program that never returns is never joinable, and this is reached
        // only when the watch loop is torn down — at which point the process is
        // going away with it.
        if ( Worker.joinable() )
        {
            Worker.detach();
        }
    }

    void Start ( Volt::Backend::Jit::JitBackend &Jit, std::span<const std::string_view> Args )
    {
        if ( Worker.joinable() )
        {
            Worker.join();
        }
        bEnded.store( false, std::memory_order_relaxed );
        bStarted = true;
        Worker   = std::thread(
            [this, &Jit, Args] ()
            {
                Result = Jit.Run( Args );
                bEnded.store( true, std::memory_order_release );
            } );
    }

    // Started, and has not returned. What decides whether an accepted reload
    // lands in a live program or starts a new one.
    [[nodiscard]] bool Running () const
    {
        return bStarted and not bEnded.load( std::memory_order_acquire );
    }

    // The result of a run that has just ended, once. Returns nothing while the
    // program is still going, and nothing again for a run already reported —
    // the watch loop asks every poll and wants to say "exit N" one time.
    [[nodiscard]] std::optional<Volt::Backend::RunResult> Reap ()
    {
        if ( not bStarted or not bEnded.load( std::memory_order_acquire ) )
        {
            return std::nullopt;
        }
        Worker.join();
        bStarted = false;
        return Result;
    }

private:

    std::thread Worker;

    // Written by the worker as its last act, read by the watch loop every
    // poll. Release/acquire rather than relaxed because Result is published
    // through it.
    std::atomic<bool> bEnded{ false };

    bool bStarted = false;
    Volt::Backend::RunResult Result;
};

} // namespace
#endif

Volt::Driver::RunResult Volt::Driver::Driver::Run ( const RunOptions &Options )
{
#ifndef VOLT_ENABLE_JIT
    static_cast<void>( Options );
    return RunResult{ .bOk     = false,
                      .Code    = 1,
                      .Message = "This build of volt was configured without the JIT (enable_jit=false); "
                                 "`volt run` is unavailable" };
#else
    if ( Options.Target != "native" )
    {
        return RunResult{
            .bOk = false, .Code = 1, .Message = "Unsupported --target '" + Options.Target + "': only 'native' runs" };
    }

    Backend::Jit::JitOptions JitOpts;
    JitOpts.Dylibs           = Options.Dylibs;
    JitOpts.OptLevel         = Options.OptLevel;
    JitOpts.bPerUnitModules  = Options.bPerUnitModules;
    JitOpts.bIndirectLinkage = Options.bIndirectLinkage;
    JitOpts.bLazyCompilation = Options.bLazyCompilation;

    // The stdlib, compiled once into a shared object and loaded rather than
    // JIT-compiled on every run. This is the single biggest thing `volt run`
    // costs without it: the stdlib is ~90 functions and dominates
    // materialisation, while the script being run is usually a handful.
    //
    // The artifact has to be the *shared* one, and it has to be the one built
    // with bDefineSlotAccessor: JIT-ed code reaches the unwind slots through
    // __volt_unwind_slots, the stdlib's own native code reaches them by TLS
    // relocation, and they are only the same storage because that accessor is
    // defined inside this artifact (UnwindTransport.hpp).
    //
    // Missing artifact is not a failure: SkipUnitsBelow stays 0 and the stdlib
    // is emitted into the module like anything else, which is slower and just
    // as correct.
    BuildOptions ArtifactOpts;
    ArtifactOpts.StdlibArtifactKind     = "shared";
    ArtifactOpts.OptLevel               = Options.OptLevel;
    ArtifactOpts.bStdlibArtifactNoCache = Options.bStdlibArtifactNoCache;
    ArtifactOpts.bStdlibArtifactFresh   = Options.bStdlibArtifactFresh;
    ArtifactOpts.bVerbose               = Options.bVerbose;

    if ( const std::optional<std::string> Artifact = EnsureStdlibArtifact( *this, ArtifactOpts ) )
    {
        JitOpts.Dylibs.insert( JitOpts.Dylibs.begin(), *Artifact );
        JitOpts.SkipUnitsBelow = static_cast<std::uint32_t>( StdlibUnitCount() );
    }

    Backend::Jit::JitBackend JitImpl;
    JitImpl.SetOptions( std::move( JitOpts ) );

    const std::vector<Backend::UnitView> Views = MakeBackendViews();
    const Backend::BackendInput BackendIn{
        .Types = &MutableLayouts(), .Units = Views, .StdlibUnitCount = static_cast<std::uint32_t>( StdlibUnitCount() ) };

    JitImpl.Begin( BackendIn );

    // A unit that fails leaves the emission's own sink set, so every later
    // EmitUnit is a guarded no-op; Finalize() is what surfaces the message.
    for ( const Backend::UnitView &Unit : Views )
    {
        static_cast<void>( JitImpl.EmitUnit( Unit ) );
    }

    const Backend::EmitResult Emitted = JitImpl.Finalize();
    if ( Emitted.Status != Backend::EEmitStatus::Ok )
    {
        return RunResult{ .bOk = false, .Code = 1, .Message = "Finalize failed: " + Emitted.Message };
    }

    std::vector<std::string_view> Args;
    Args.reserve( Options.ProgramArgs.size() );
    for ( const std::string &Arg : Options.ProgramArgs )
    {
        Args.emplace_back( Arg );
    }

    if ( not Options.bWatch )
    {
        const Backend::RunResult Ran = JitImpl.Run( Args );
        return RunResult{ .bOk = Ran.bOk, .Code = Ran.Code, .Message = Ran.Message };
    }

    // --- Watch -------------------------------------------------------------
    //
    // This Driver stays alive for the whole loop: it owns the type store the
    // running code was laid out against, and every reload is judged against
    // that, not against the previous reload.
    //
    // The program goes on its own thread and the loop starts watching straight
    // away, without waiting for it. A program that returns in a millisecond is
    // reaped on the first poll and reads exactly as it always did; a program
    // that does not return is the reason any of this exists.
    ProgramThread Program;
    Program.Start( JitImpl, Args );

    Core::FLogger::Info( "watching for changes (Ctrl-C to stop)", "watch" );
    Core::FLogger::Flush();

    const auto ReportIfEnded = [&Program] ()
    {
        if ( const std::optional<Backend::RunResult> Ended = Program.Reap() )
        {
            Core::FLogger::Info( Ended->bOk ? "exit " + std::to_string( Ended->Code ) : "run failed: " + Ended->Message,
                                 "watch" );
            Core::FLogger::Flush();
        }
    };

    std::map<std::string, Stamp> Seen;
    for ( std::size_t Index = StdlibUnitCount(); Index < UnitCount(); ++Index )
    {
        Seen[Unit( Index ).Path] = StampOf( Unit( Index ).Path );
    }

    while ( true )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( PollInterval ) );
        ReportIfEnded();

        std::string Changed;
        for ( auto &[Path, Was] : Seen )
        {
            if ( const Stamp Now = StampOf( Path ); Now != Was and Now.Size != 0 )
            {
                Was     = Now;
                Changed = Path;
                break;
            }
        }
        if ( Changed.empty() )
        {
            continue;
        }

        Core::FLogger::Info( "changed: " + Changed, "watch" );
        Core::FLogger::Flush();

        // A whole new Driver, not a re-entry into this one. Recompiling one
        // file means re-running the front end, and the front end's output is a
        // type store with ids of its own; the running program keeps the old
        // one, and the difference between the two is exactly what the backend
        // refuses or accepts on.
        Driver Fresh;
        const CompileResult Recompiled = Options.WatchManifest.empty()
                                             ? Fresh.CompileFiles( Options.WatchInputs, Options.CacheOpts )
                                             : Fresh.CompileCircuit( Options.WatchManifest, Options.CacheOpts );
        if ( Fresh.HasErrors() )
        {
            Fresh.RenderDiagnostics( std::cerr );
            Core::FLogger::Error( std::to_string( Recompiled.Errors ) + " error(s) — keeping the running code", "watch" );
            Core::FLogger::Flush();
            continue;
        }

        const std::vector<Backend::UnitView> FreshViews = Fresh.MakeBackendViews();
        const Backend::BackendInput FreshIn{ .Types           = &Fresh.MutableLayouts(),
                                             .Units           = FreshViews,
                                             .StdlibUnitCount = static_cast<std::uint32_t>( Fresh.StdlibUnitCount() ) };

        const auto Found = std::find_if( FreshViews.begin(), FreshViews.end(),
                                         [&Changed] ( const Backend::UnitView &View ) { return View.Path == Changed; } );
        if ( Found == FreshViews.end() )
        {
            Core::FLogger::Error( "'" + Changed + "' is no longer part of this build", "watch" );
            Core::FLogger::Flush();
            continue;
        }

        // Asked *before* the emission, and asked again after the patch, because
        // neither moment alone is the right one. A program can end during the
        // second the replacement takes to compile, and a program can end
        // *because of* the patch — a loop whose condition just became false is
        // the smallest example, and it exits before the store returns. Either
        // answer being yes means the reload was aimed at a live process, which
        // is the only question the restart below turns on.
        const bool bWasLive = Program.Running();

        const Backend::ReloadResult Reloaded = JitImpl.Reload( FreshIn, *Found );
        if ( Reloaded.Status != Backend::EReloadStatus::Ok )
        {
            const std::string What = Reloaded.Status == Backend::EReloadStatus::Refused ? "refused" : "failed";
            Core::FLogger::Error( "reload " + What + ": " + Reloaded.Message, "watch" );
            Core::FLogger::Error( "restart `volt run --watch` to pick the change up", "watch" );
            Core::FLogger::Flush();
            continue;
        }

        const std::string Patched = "reloaded " + std::to_string( Reloaded.PatchedSymbols ) + " symbol(s) — ";

        // The two halves of a hot reload, and which one happens is decided by
        // the program rather than by an option.
        //
        // Still running: nothing restarts it. The slots it calls through now
        // point at the new bodies, so the change takes effect at the next call
        // — the next request a server serves, the next turn of an event loop —
        // and everything the program had built up in memory survives it.
        //
        // Already returned: the same slots, but nobody is reading them, so the
        // program is started again to execute the change. That is what `volt
        // run --watch` has always done, and for a script it remains the only
        // sensible reading of "reload".
        if ( bWasLive or Program.Running() )
        {
            Core::FLogger::Info( Patched + "the running program picks them up at its next call", "watch" );
            Core::FLogger::Flush();
            continue;
        }

        Core::FLogger::Info( Patched + "running again", "watch" );
        Core::FLogger::Flush();
        Program.Start( JitImpl, Args );
    }
#endif
}
