// StdlibArtifact.cpp — issue #61's native stdlib artifact cache build mode:
// the one place a caller outside this module (Driver::Build(), in
// Driver/Private/DriverBuild.cpp) reaches for a precompiled stdlib
// archive/.so. Every raw LLVM call this needs (process execution, temp
// files, the host triple) stays here — the public surface (LlvmEmitter.hpp)
// hands back plain data, exactly like LlvmBackend itself.

#include "Volt/BackendLLVM/LlvmEmitter.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/Program.h>
#include <llvm/TargetParser/Host.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

namespace fs = std::filesystem;

// `ar rcs Target ObjectPath`, published atomically (tmp file + rename — the
// same discipline Driver::WriteFrontendCache uses for frontend.cache).
[[nodiscard]] bool ArchiveObject ( std::string_view ObjectPath, const fs::path &Target )
{
    const llvm::ErrorOr<std::string> Ar = llvm::sys::findProgramByName( "ar" );
    if ( not Ar )
    {
        return false;
    }

    const fs::path Tmp       = Target.parent_path() / ( "tmp." + std::to_string( static_cast<long long>( ::getpid() ) ) + ".a" );
    const std::string TmpStr = Tmp.string();
    const std::string ObjStr( ObjectPath );
    const std::vector<llvm::StringRef> Args{ *Ar, "rcs", TmpStr, ObjStr };

    std::string ErrorMessage;
    bool bFailed     = false;
    const int Result = llvm::sys::ExecuteAndWait( *Ar, Args, std::nullopt, {}, 0, 0, &ErrorMessage, &bFailed );
    std::error_code Ec;
    if ( bFailed or Result != 0 )
    {
        fs::remove( Tmp, Ec );
        return false;
    }

    fs::rename( Tmp, Target, Ec );
    if ( Ec )
    {
        fs::remove( Tmp, Ec );
        return false;
    }
    return true;
}

// A best-effort `.meta` symbol manifest, dumped straight off the produced
// artifact via `nm` — advisory only (PROGRESS-issue-61.md's Phase 3
// write-up): a missing one never blocks linking, only a future diagnostic's
// ability to tell "cache-format bug" from "genuine undefined reference"
// apart, so any failure here is silent.
void WriteSymbolManifest ( const fs::path &Artifact, const fs::path &Meta )
{
    const llvm::ErrorOr<std::string> Nm = llvm::sys::findProgramByName( "nm" );
    if ( not Nm )
    {
        return;
    }
    llvm::SmallString<128> OutFile;
    if ( llvm::sys::fs::createTemporaryFile( "volt-stdlib-meta", "txt", OutFile ) )
    {
        return;
    }
    const llvm::FileRemover Cleanup( OutFile.str() );

    const std::string ArtifactStr = Artifact.string();
    const std::vector<llvm::StringRef> Args{ *Nm, "--defined-only", "-g", ArtifactStr };
    const std::optional<llvm::StringRef> Redirects[3] = { std::nullopt, llvm::StringRef( OutFile ), std::nullopt };
    std::string ErrorMessage;
    if ( llvm::sys::ExecuteAndWait( *Nm, Args, std::nullopt, Redirects, 0, 0, &ErrorMessage ) != 0 )
    {
        return;
    }
    std::error_code Ec;
    fs::copy_file( fs::path( std::string( OutFile.str() ) ), Meta, fs::copy_options::overwrite_existing, Ec );
}

} // namespace

std::string Volt::Backend::Llvm::DefaultTargetTriple ()
{
    return llvm::sys::getDefaultTargetTriple();
}

bool Volt::Backend::Llvm::BuildStdlibArtifact ( const BackendInput &Build,
                                                std::uint8_t OptLevel,
                                                bool bLto,
                                                bool bShared,
                                                std::string_view TargetPath,
                                                std::string_view MetaPath )
{
    EmitOptions Opts;
    Opts.OptLevel = OptLevel;
    Opts.bLto     = bLto;
    Opts.EntryFunction.clear();
    Opts.EntrySymbol.clear(); // a library build: no CRT entry point.

    llvm::SmallString<128> TempFile;
    if ( llvm::sys::fs::createTemporaryFile( "volt-stdlib", bShared ? "so" : "o", TempFile ) )
    {
        return false;
    }
    const llvm::FileRemover Cleanup( TempFile.str() );

    Opts.Stage         = bShared ? EEmitStage::Link : EEmitStage::Object;
    Opts.bSharedOutput = bShared;
    Opts.OutputPath    = TempFile.str().str();

    LlvmBackend Impl;
    Impl.SetOptions( Opts );
    std::unique_ptr<IBackend> TheBackend = MakeBackend( std::move( Impl ) );
    TheBackend->Begin( Build );
    for ( const UnitView &Unit : Build.Units )
    {
        static_cast<void>( TheBackend->EmitUnit( Unit ) );
    }
    const EmitResult Emitted = TheBackend->Finalize();
    if ( Emitted.Status != EEmitStatus::Ok )
    {
        return false;
    }

    const fs::path Target( TargetPath );
    bool bOk = false;
    if ( bShared )
    {
        std::error_code Ec;
        fs::rename( TempFile.str().str(), Target, Ec );
        bOk = not Ec;
    }
    else
    {
        bOk = ArchiveObject( TempFile.str(), Target );
    }

    if ( bOk and not MetaPath.empty() )
    {
        WriteSymbolManifest( Target, fs::path( std::string( MetaPath ) ) );
    }
    return bOk;
}
