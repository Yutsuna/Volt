// LlvmLifecycle.cpp — Begin / EmitUnit / Finalize.
//
// The shape of a build, and nothing else: set up the module and the host target,
// declare everything reachable, define it unit by unit, then drain, verify,
// optimise and emit. Every step delegates; the ordering *is* the content of this
// file, and it is load-bearing at three points in particular —
//
//   - DeclareAll runs before any body, so a body in the first unit calling
//     something declared in the last resolves with no fixup pass;
//   - the monomorphiser drains before the entry point is emitted, so the entry
//     point can itself be the thing that forced an instantiation;
//   - the verifier runs after that, so the shim is proved well-formed like any
//     other function.

#include "Core/LlvmBackendState.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>

#include <string>
#include <system_error>

void Volt::Backend::Llvm::LlvmBackend::Begin ( const BackendInput &Input )
{
    Impl->Build           = &Input;
    Impl->Services.Build  = &Input;

    if ( Input.Types == nullptr )
    {
        static_cast<void>( Impl->Fail( "llvm: the build carries no TypeStore" ) );
        return;
    }

    Impl->Layouts.emplace( *Input.Types );

    // One llvm::Module per build: the simplest correct thing. Per-unit modules
    // plus ThinLTO is a later optimisation behind this same interface. The entry
    // module is last in circuit link order, so it names the module.
    const std::string_view Name = Input.Units.empty() ? std::string_view{ "volt" } : Input.Units.back().Module;

    std::string Error;
    if ( not Impl->Ctx.InitTarget( Name, Error ) )
    {
        static_cast<void>( Impl->Fail( std::move( Error ) ) );
        return;
    }

    // Declare before defining anything: a body emitted in the first unit may call
    // something declared in the last, and one pass over the store means that
    // resolves immediately instead of needing a fixup pass.
    DeclareAll( Impl->Services );
}

Volt::Backend::EEmitStatus Volt::Backend::Llvm::LlvmBackend::EmitUnit ( const UnitView &Unit )
{
    if ( Impl->Failed() )
    {
        return EEmitStatus::Error;
    }

    if ( Impl->Build == nullptr or Impl->Build->Types == nullptr )
    {
        return Impl->Fail( "llvm: unit '" + std::string( Unit.Path ) +
                           "' reached the backend with no build input or type store" );
    }

    if ( Unit.Ast == nullptr or Unit.Values == nullptr or Unit.Callees == nullptr or Unit.Scopes == nullptr )
    {
        return Impl->Fail( "llvm: unit '" + std::string( Unit.Path ) + "' reached the backend with no sema output" );
    }

    // A stdlib unit under a precompiled build never gets a body here: it is
    // already declared (DeclareAll runs over the whole TypeStore regardless of
    // unit) and its definition is expected from the linked archive/.so instead —
    // the same "declared, never defined" shape @[External] members already have.
    if ( Impl->Options.bStdlibPrecompiled and Unit.Ordinal < Impl->Build->StdlibUnitCount )
    {
        return EEmitStatus::Ok;
    }

    DefineAll( Impl->Services, Unit );
    return Impl->Failed() ? EEmitStatus::Error : EEmitStatus::Ok;
}

Volt::Backend::EmitResult Volt::Backend::Llvm::LlvmBackend::Finalize ()
{
    const auto MakeFailure = [this] ()
    { return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Impl->Diag.Message() }; };

    if ( Impl->Failed() )
    {
        return MakeFailure();
    }

    // Every unit is defined by now, so every instantiation a concrete body could
    // ever discover has been enqueued. A drained body can itself enqueue more — a
    // generic method calling another generic method — so this drains to a
    // fixpoint rather than once.
    Impl->Mono->Drain();
    if ( Impl->Failed() )
    {
        return MakeFailure();
    }

    // After the drain, so the entry point can itself be the thing that forced an
    // instantiation, and before the verifier, which is what proves the shim is
    // well formed like any other function.
    if ( not EmitEntryPoint( Impl->Services ) )
    {
        return MakeFailure();
    }

    // A broken module past this point is an emitter bug (rules/core-ast.md
    // guarantees the middle-end never hands over anything the module verifier
    // could reject), never a Volt source error — VerifyModule names the offending
    // function rather than the caller guessing.
    if ( not Impl->Pipeline->VerifyModule() )
    {
        return MakeFailure();
    }

    // Runs even at -O0: PassBuilder's O0 pipeline is the minimal
    // semantically-required set, principally mem2reg, and the emitter itself
    // never builds SSA (llvm.md, "every alloca goes in the entry block") — it
    // depends on this pass to promote them back to registers.
    Impl->Pipeline->RunOptimizationPipeline();

    const std::string ModuleName = Impl->Ctx.Mod().getName().str();
    const std::string BaseName   = ModuleName.empty() ? std::string( "volt" ) : ModuleName;

    if ( Impl->Options.Stage == EEmitStage::Ir )
    {
        const std::string Path = Impl->Options.OutputPath.empty() ? BaseName + ".ll" : Impl->Options.OutputPath;
        if ( not Impl->Pipeline->EmitIrFile( Path ) )
        {
            return MakeFailure();
        }
        return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = Path, .Message = {} };
    }

    if ( Impl->Options.Stage == EEmitStage::Object )
    {
        const std::string Path = Impl->Options.OutputPath.empty() ? BaseName + ".o" : Impl->Options.OutputPath;
        if ( not Impl->Pipeline->EmitObjectFile( Path ) )
        {
            return MakeFailure();
        }
        return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = Path, .Message = {} };
    }

    // Link: the object file is a temporary intermediate — nothing downstream of
    // the linker ever reads it — removed once the link either succeeds or fails.
    llvm::SmallString<128> TempObject;
    if ( const std::error_code Error = llvm::sys::fs::createTemporaryFile( BaseName, "o", TempObject ) )
    {
        static_cast<void>( Impl->Fail( "llvm: could not create a temporary object file: " + Error.message() ) );
        return MakeFailure();
    }
    const llvm::FileRemover ObjectCleanup( TempObject.str() );

    if ( not Impl->Pipeline->EmitObjectFile( TempObject.str() ) )
    {
        return MakeFailure();
    }

    const std::string DefaultName = Impl->Options.bSharedOutput ? BaseName + ".so" : std::string( "a.out" );
    const std::string OutputPath  = Impl->Options.OutputPath.empty() ? DefaultName : Impl->Options.OutputPath;

    const bool bLinked = Impl->Options.bSharedOutput ? Impl->Linker->LinkSharedLibrary( TempObject.str(), OutputPath )
                                                     : Impl->Linker->LinkExecutable( TempObject.str(), OutputPath );
    if ( not bLinked )
    {
        return MakeFailure();
    }

    return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = OutputPath, .Message = {} };
}
