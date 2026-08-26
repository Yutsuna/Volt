// DriverBuild.cpp — Driver::Build(): the one place --target resolves to a
// concrete IBackend (issue #61). Everything backend-specific, including
// every raw LLVM header, stays inside this TU's VOLT_ENABLE_LLVM branch —
// Driver.hpp itself never mentions Backend::Llvm, so a caller (BuildCommand
// .cpp) never needs to either.

#include "Volt/Driver/Driver.hpp"

#ifdef VOLT_ENABLE_LLVM
    #include "Volt/BackendCore/TargetBackend.hpp"
    #include "Volt/BackendLLVM/LlvmEmitter.hpp"
    #include "Volt/Core/Log/Logger.hpp"
    #include "Volt/Core/Support/ContentHash.hpp"

    #include <filesystem>
    #include <memory>
    #include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

#ifdef VOLT_ENABLE_LLVM

namespace
{

namespace fs = std::filesystem;

// Compile just the stdlib in a throwaway Driver — the same shape as
// WriteFrontendCache's own "Warm" driver — so the artifact this produces
// holds only stdlib-owned symbols. Null on a key mismatch or a compile
// error.
[[nodiscard]] std::unique_ptr<Volt::Driver::Driver> CompileIsolatedStdlib ( std::uint64_t ExpectedKey )
{
    auto Isolated = std::make_unique<Volt::Driver::Driver>();
    if ( Isolated->FrontendCacheKey() != ExpectedKey )
    {
        return nullptr; // paranoia: only precompile a stdlib the caller's own key would trust.
    }
    static_cast<void>( Isolated->CompileFiles( {} ) );
    return Isolated->HasErrors() ? nullptr : std::move( Isolated );
}

// Ensures a precompiled native stdlib artifact for TheDriver's already-
// compiled stdlib exists — reusing one on disk, or building/refreshing it via
// CompileIsolatedStdlib + Backend::Llvm::BuildStdlibArtifact when missing or
// stale (Options.bStdlibArtifactFresh) — and returns its path.
//
// Never a hard failure: a disabled cache, an empty stdlib, an unrecognised
// artifact kind, no cache directory (neither XDG_CACHE_HOME nor HOME set), or
// the inner build itself failing all return nullopt — the caller's fallback
// is simply not setting EmitOptions::bStdlibPrecompiled, i.e. an ordinary
// full build.
[[nodiscard]] std::optional<std::string> EnsureStdlibArtifactImpl ( Volt::Driver::Driver &TheDriver,
                                                                    const Volt::Driver::BuildOptions &Options )
{
    if ( Options.bStdlibArtifactNoCache or TheDriver.StdlibUnitCount() == 0 )
    {
        return std::nullopt;
    }
    if ( Options.StdlibArtifactKind != "static" and Options.StdlibArtifactKind != "shared" )
    {
        return std::nullopt; // Build() itself already reported the unsupported kind.
    }
    const bool bShared = Options.StdlibArtifactKind == "shared";

    const fs::path CacheDir = Volt::Driver::StdlibCacheDir( TheDriver.FrontendCacheKey() );
    if ( CacheDir.empty() )
    {
        return std::nullopt;
    }

    const std::string Triple      = Volt::Backend::Llvm::DefaultTargetTriple();
    const std::uint64_t NativeKey = Volt::Driver::ComputeNativeCacheKey(
        TheDriver.FrontendCacheKey(), Triple, std::to_string( Options.OptLevel ), Options.StdlibArtifactKind, Options.bLto );

    // Phase 5: log cache keys when verbose mode is enabled
    if ( Options.bVerbose )
    {
        Volt::Core::FLogger::Info( "Native cache key: " + Volt::Core::ToHex( NativeKey ), "driver" );
    }

    const fs::path NativeDir = CacheDir / "native";
    const fs::path Artifact  = NativeDir / ( Volt::Core::ToHex( NativeKey ) + ( bShared ? ".so" : ".a" ) );
    const fs::path Meta      = NativeDir / ( Volt::Core::ToHex( NativeKey ) + ".meta" );

    std::error_code Ec;
    if ( not Options.bStdlibArtifactFresh and fs::is_regular_file( Artifact, Ec ) )
    {
        return Artifact.string();
    }

    fs::create_directories( NativeDir, Ec );

    const std::unique_ptr<Volt::Driver::Driver> Isolated = CompileIsolatedStdlib( TheDriver.FrontendCacheKey() );
    if ( Isolated == nullptr )
    {
        return std::nullopt;
    }

    const std::vector<Volt::Backend::UnitView> Views = Isolated->MakeBackendViews();
    const Volt::Backend::BackendInput BackendIn{ .Types = &Isolated->MutableLayouts(), .Units = Views };

    // Built beside the real name, then renamed into place. Several compilations
    // can run at once — a parallel test suite is the ordinary case — and they
    // all derive the same key, so they all decide to build the same file. A
    // reader that opens it midway through a write sees a truncated object and
    // fails in a way that has nothing to do with what it was compiling. rename
    // within one directory is atomic, so a reader sees either no file or a
    // finished one, and a redundant build is only wasted work.
    const std::string Tag      = std::to_string( static_cast<long long>( ::getpid() ) );
    const fs::path PendingArt  = NativeDir / ( Volt::Core::ToHex( NativeKey ) + "." + Tag + ".pending" );
    const fs::path PendingMeta = NativeDir / ( Volt::Core::ToHex( NativeKey ) + "." + Tag + ".pending.meta" );

    if ( not Volt::Backend::Llvm::BuildStdlibArtifact( BackendIn, Options.OptLevel, Options.bLto, bShared, PendingArt.string(),
                                                       PendingMeta.string() ) )
    {
        fs::remove( PendingArt, Ec );
        fs::remove( PendingMeta, Ec );
        return std::nullopt;
    }

    fs::rename( PendingArt, Artifact, Ec );
    if ( Ec )
    {
        fs::remove( PendingArt, Ec );
        fs::remove( PendingMeta, Ec );
        return std::nullopt;
    }
    fs::rename( PendingMeta, Meta, Ec );

    return Artifact.string();
}

} // namespace

// Shared with DriverRun.cpp: `volt run` wants exactly the same artifact, in its
// shared form, for exactly the same reason — the stdlib is already compiled, so
// neither path should compile it again.
std::optional<std::string> Volt::Driver::EnsureStdlibArtifact ( Driver &TheDriver, const BuildOptions &Options )
{
    return EnsureStdlibArtifactImpl( TheDriver, Options );
}

#endif // VOLT_ENABLE_LLVM

Volt::Driver::BuildResult Volt::Driver::Driver::Build ( const BuildOptions &Options )
{
#ifndef VOLT_ENABLE_LLVM
    static_cast<void>( Options );
    return BuildResult{ .bOk      = false,
                        .Artifact = {},
                        .Message  = "This build of volt was configured without LLVM (VOLT_ENABLE_LLVM=OFF); "
                                    "native code generation is unavailable" };
#else
    if ( Options.Target != "native" )
    {
        return BuildResult{ .bOk      = false,
                            .Artifact = {},
                            .Message  = "Unsupported --target '" + Options.Target + "': only 'native' is implemented" };
    }

    Backend::Llvm::EmitOptions EmitOpts;
    EmitOpts.OutputPath  = Options.OutputPath;
    EmitOpts.OptLevel    = Options.OptLevel;
    EmitOpts.bLto        = Options.bLto;
    EmitOpts.EntrySymbol = Options.EntrySymbol;

    std::string EmitLower = Options.Emit;
    std::ranges::transform( EmitLower, EmitLower.begin(), [] ( unsigned char C ) { return static_cast<char>( std::tolower( C ) ); } );

    if ( EmitLower == "ir" )
    {
        EmitOpts.Stage = Backend::Llvm::EEmitStage::Ir;
    }
    else if ( EmitLower == "obj" )
    {
        EmitOpts.Stage = Backend::Llvm::EEmitStage::Object;
    }
    else if ( not EmitLower.empty() )
    {
        return BuildResult{
            .bOk = false, .Artifact = {}, .Message = "Unsupported --emit '" + Options.Emit + "': expected 'ir' or 'obj'" };
    }

    // The native stdlib artifact cache only applies to a full link:
    // `--emit ir`/`--emit obj` stop before ExtraLinkInputs would ever reach
    // the linker, and a stdlib body skipped here would simply never be
    // defined anywhere in that intermediate artifact.
    if ( EmitOpts.Stage == Backend::Llvm::EEmitStage::Link )
    {
        if ( Options.StdlibArtifactKind != "static" and Options.StdlibArtifactKind != "shared" )
        {
            return BuildResult{ .bOk      = false,
                                .Artifact = {},
                                .Message  = "Unsupported --stdlib-artifact '" + Options.StdlibArtifactKind +
                                           "': expected 'static' or 'shared'" };
        }
        if ( const std::optional<std::string> Artifact = EnsureStdlibArtifactImpl( *this, Options ) )
        {
            EmitOpts.bStdlibPrecompiled = true;
            EmitOpts.ExtraLinkInputs.push_back( *Artifact );
        }
    }

    Backend::Llvm::LlvmBackend LlvmImpl;
    LlvmImpl.SetOptions( EmitOpts );
    std::unique_ptr<Backend::IBackend> TheBackend = Backend::MakeBackend( std::move( LlvmImpl ) );

    const std::vector<Backend::UnitView> Views = MakeBackendViews();
    const Backend::BackendInput BackendIn{
        .Types = &MutableLayouts(), .Units = Views, .StdlibUnitCount = static_cast<std::uint32_t>( StdlibUnitCount() ) };
    TheBackend->Begin( BackendIn );

    // A unit that fails leaves State::Failed() set, so every later EmitUnit
    // is a guarded no-op; Finalize() is still what surfaces the real message.
    for ( const Backend::UnitView &Unit : Views )
    {
        static_cast<void>( TheBackend->EmitUnit( Unit ) );
    }

    const Backend::EmitResult Emitted = TheBackend->Finalize();
    if ( Emitted.Status != Backend::EEmitStatus::Ok )
    {
        return BuildResult{ .bOk = false, .Artifact = {}, .Message = "Finalize failed: " + Emitted.Message };
    }

    return BuildResult{ .bOk = true, .Artifact = Emitted.Artifact, .Message = {} };
#endif
}
