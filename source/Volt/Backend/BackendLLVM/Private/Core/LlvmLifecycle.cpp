// LlvmLifecycle.cpp — Begin / EmitUnit / Finalize.
//
// The shape of an ahead-of-time build. Emission itself is one call into
// BackendLlvmIr; what is left here is the tail that turns the finished module
// into an artifact — verify, optimise, then `.ll`, `.o`, or a link — and the
// decision of *which* artifact, which is the only thing `--emit` selects.
//
// The one piece of real logic is the skip line: when the stdlib was already
// compiled into an artifact this build will link against, its units need no
// bodies emitted. They still get their inline-eligible ones, because an
// optimised build inlines across that boundary — which is exactly the
// distinction IrOptions draws between SkipUnitsBelow and
// bDefineInlineEligibleBelow.

#include "Core/LlvmBackendState.hpp"

#include "Volt/BackendLlvmIr/LlvmAccess.hpp"

#include "Volt/Core/Support/PhaseTimer.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>

#include <memory>
#include <string>
#include <system_error>
#include <utility>

void Volt::Backend::Llvm::LlvmBackend::Begin ( const BackendInput &Input )
{
    Impl->Build = &Input;

    if ( Input.Types == nullptr )
    {
        static_cast<void>( Impl->Fail( "llvm: the build carries no TypeStore" ) );
        return;
    }

    Ir::IrOptions Gen;
    Gen.SkipUnitsBelow = Impl->Options.bStdlibPrecompiled ? Input.StdlibUnitCount : 0U;

    // The AOT build links the precompiled stdlib but still wants its
    // inline-eligible bodies in the module, so -O2 can inline across the
    // boundary. A JIT wants the skip without that exception.
    Gen.bDefineInlineEligibleBelow = true;

    Gen.EntryFunction = Impl->Options.EntryFunction;
    Gen.EntrySymbol   = Impl->Options.EntrySymbol;
    Gen.bDebugInfo    = Impl->Options.bDebugInfo;

    Gen.bDefineSlotAccessor     = Impl->Options.bDefineSlotAccessor;
    Gen.bRetainMergeableBodies  = Impl->Options.bRetainMergeableBodies;

    // Verification stays in this module's own pipeline, where the failure is
    // reported with the offending function named.
    Gen.bVerify = false;

    Impl->Gen.emplace( std::move( Gen ) );
    Impl->Gen->Begin( Input );
    if ( Impl->Gen->Failed() )
    {
        return;
    }

    // Borrowed, not owned: the generator keeps the module and this tail works
    // over it in place.
    Impl->Services.Build   = &Input;
    Impl->Services.Options = &Impl->Options;
    Impl->Services.Diag    = &Impl->Diag;
    Impl->Services.Mod     = &Ir::ModuleOf( *Impl->Gen );
    Impl->Services.Machine = Ir::MachineOf( *Impl->Gen );

    Impl->Pipeline = std::make_unique<TargetPipeline>( Impl->Services );
    Impl->Linker   = std::make_unique<LinkerDriver>( Impl->Services );
}

Volt::Backend::EEmitStatus Volt::Backend::Llvm::LlvmBackend::EmitUnit ( const UnitView &Unit )
{
    if ( Impl->Failed() or not Impl->Gen.has_value() )
    {
        return EEmitStatus::Error;
    }
    return Impl->Gen->EmitUnit( Unit );
}

Volt::Backend::EmitResult Volt::Backend::Llvm::LlvmBackend::Finalize ()
{
    const auto MakeFailure = [this] ()
    { return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Impl->Message() }; };

    if ( Impl->Failed() or not Impl->Gen.has_value() )
    {
        return MakeFailure();
    }

    // Drains the monomorphiser to a fixpoint and caps the module with its entry
    // point. Past this the module is complete and never grows again.
    if ( Impl->Gen->Finish() != EEmitStatus::Ok )
    {
        return MakeFailure();
    }

    // A broken module past this point is an emitter bug (rules/core-ast.md
    // guarantees the middle-end never hands over anything the module verifier
    // could reject), never a Volt source error — VerifyModule names the offending
    // function rather than the caller guessing.
    {
        const Volt::Core::PhaseScope Timing( "backend.verify" );
        if ( not Impl->Pipeline->VerifyModule() )
        {
            return MakeFailure();
        }
    }

    // Runs even at -O0: PassBuilder's O0 pipeline is the minimal
    // semantically-required set, principally mem2reg, and the emitter itself
    // never builds SSA (llvm.md, "every alloca goes in the entry block") — it
    // depends on this pass to promote them back to registers.
    {
        const Volt::Core::PhaseScope Timing( "backend.optimize" );
        Impl->Pipeline->RunOptimizationPipeline();
    }

    const std::string ModuleName = Impl->Services.Mod->getName().str();
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
