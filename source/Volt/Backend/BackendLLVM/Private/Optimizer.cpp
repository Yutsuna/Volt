// Optimizer.cpp — verify, optimise, and emit the finished module (llvm.md,
// "Optimisation & emission"). Everything here runs once, in Finalize(),
// after every unit is defined and the monomorphiser has drained to a
// fixpoint: the module is complete and never grows again past this point.

#include "LlvmState.hpp"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

bool Volt::Backend::Llvm::LlvmBackend::State::VerifyModule ()
{
    // Volt's own contract (rules/core-ast.md) guarantees the middle-end never
    // hands the emitter something it cannot type or resolve; a broken module
    // at this point is an emitter bug, not a Volt source error, so the
    // offending function is named rather than the diagnostic being left to
    // whatever llvm::verifyModule prints on its own.
    std::string Report;
    llvm::raw_string_ostream Stream( Report );
    if ( not llvm::verifyModule( *Mod, &Stream ) )
    {
        return true;
    }

    for ( const llvm::Function &Fn : *Mod )
    {
        std::string FunctionReport;
        llvm::raw_string_ostream FunctionStream( FunctionReport );
        if ( llvm::verifyFunction( Fn, &FunctionStream ) )
        {
            static_cast<void>(
                Fail( "llvm: module verification failed in '" + std::string( Fn.getName() ) + "': " + FunctionStream.str() ) );
            return false;
        }
    }

    // The verifier found the module broken but no single function is at
    // fault (a malformed global, an ill-typed metadata node, ...) — report
    // the module-level text verbatim rather than guessing which function.
    static_cast<void>( Fail( "llvm: module verification failed: " + Stream.str() ) );
    return false;
}

namespace
{

// `bLto` selects full optimisation rather than a genuine cross-module LTO
// run: one llvm::Module already holds the whole build (llvm.md's "one
// Module per build to start"), so there is nothing outside this module to
// link against yet. A later per-unit-module change is what would make
// this a real ThinLTO pipeline (PLAN_LLVM_TIER1.md, phase 1 notes).
[[nodiscard]] llvm::OptimizationLevel OptimizationLevelOf ( std::uint8_t OptLevel, bool bLto )
{
    if ( bLto or OptLevel >= 3 )
    {
        return llvm::OptimizationLevel::O3;
    }
    if ( OptLevel == 2 )
    {
        return llvm::OptimizationLevel::O2;
    }
    if ( OptLevel == 1 )
    {
        return llvm::OptimizationLevel::O1;
    }
    return llvm::OptimizationLevel::O0;
}

} // namespace

void Volt::Backend::Llvm::LlvmBackend::State::RunOptimizationPipeline () const
{
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB( Machine.get() );

    PB.registerModuleAnalyses( MAM );
    PB.registerCGSCCAnalyses( CGAM );
    PB.registerFunctionAnalyses( FAM );
    PB.registerLoopAnalyses( LAM );
    PB.crossRegisterProxies( LAM, FAM, CGAM, MAM );

    const llvm::OptimizationLevel Level = OptimizationLevelOf( Options.OptLevel, Options.bLto );

    llvm::ModulePassManager MPM =
        Level == llvm::OptimizationLevel::O0 ? PB.buildO0DefaultPipeline( Level ) : PB.buildPerModuleDefaultPipeline( Level );
    MPM.run( *Mod, MAM );
}

bool Volt::Backend::Llvm::LlvmBackend::State::EmitIrFile ( std::string_view Path )
{
    std::error_code Error;
    llvm::raw_fd_ostream Stream( Path, Error, llvm::sys::fs::OF_Text );
    if ( Error )
    {
        static_cast<void>( Fail( "llvm: could not open '" + std::string( Path ) + "' for --emit ir: " + Error.message() ) );
        return false;
    }
    Mod->print( Stream, nullptr );
    return not Stream.has_error();
}

bool Volt::Backend::Llvm::LlvmBackend::State::EmitObjectFile ( std::string_view Path )
{
    std::error_code Error;
    llvm::raw_fd_ostream Stream( Path, Error, llvm::sys::fs::OF_None );
    if ( Error )
    {
        static_cast<void>( Fail( "llvm: could not open '" + std::string( Path ) + "' for object emission: " + Error.message() ) );
        return false;
    }

    llvm::legacy::PassManager CodegenPasses;
    if ( Machine->addPassesToEmitFile( CodegenPasses, Stream, nullptr, llvm::CodeGenFileType::ObjectFile ) )
    {
        static_cast<void>( Fail( "llvm: target '" + Machine->getTargetTriple().str() + "' cannot emit an object file" ) );
        return false;
    }

    CodegenPasses.run( *Mod );
    Stream.flush();
    return not Stream.has_error();
}
