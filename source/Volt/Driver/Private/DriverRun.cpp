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
    #include <chrono>
    #include <cstdint>
    #include <filesystem>
    #include <iostream>
    #include <map>
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

    Backend::RunResult Ran = JitImpl.Run( Args );
    if ( not Options.bWatch or not Ran.bOk )
    {
        return RunResult{ .bOk = Ran.bOk, .Code = Ran.Code, .Message = Ran.Message };
    }

    // --- Watch -------------------------------------------------------------
    //
    // This Driver stays alive for the whole loop: it owns the type store the
    // running code was laid out against, and every reload is judged against
    // that, not against the previous reload.
    Core::FLogger::Info( "exit " + std::to_string( Ran.Code ) + " — watching for changes (Ctrl-C to stop)", "watch" );
    Core::FLogger::Flush();

    std::map<std::string, Stamp> Seen;
    for ( std::size_t Index = StdlibUnitCount(); Index < UnitCount(); ++Index )
    {
        Seen[Unit( Index ).Path] = StampOf( Unit( Index ).Path );
    }

    while ( true )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( PollInterval ) );

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

        const Backend::ReloadResult Reloaded = JitImpl.Reload( FreshIn, *Found );
        if ( Reloaded.Status != Backend::EReloadStatus::Ok )
        {
            const std::string What = Reloaded.Status == Backend::EReloadStatus::Refused ? "refused" : "failed";
            Core::FLogger::Error( "reload " + What + ": " + Reloaded.Message, "watch" );
            Core::FLogger::Error( "restart `volt run --watch` to pick the change up", "watch" );
            Core::FLogger::Flush();
            continue;
        }

        Core::FLogger::Info( "reloaded " + std::to_string( Reloaded.PatchedSymbols ) + " symbol(s) — running again", "watch" );
        Core::FLogger::Flush();

        // The whole program again, entry point included: every call the new
        // bodies are reached by goes through a slot that now points at them, so
        // a second run executes the change without anything else being
        // recompiled.
        Ran = JitImpl.Run( Args );
        Core::FLogger::Info( Ran.bOk ? "exit " + std::to_string( Ran.Code ) : "run failed: " + Ran.Message, "watch" );
        Core::FLogger::Flush();
    }
#endif
}
