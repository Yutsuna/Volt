// Optimizer.cpp — PassBuilder's default pipelines.
//
// Runs even at -O0: the O0 pipeline is the minimal semantically-required set,
// principally mem2reg, and this emitter never builds SSA itself (every alloca
// goes in the entry block) — it depends on that pass to promote them back to
// registers.

#include "Target/TargetPipeline.hpp"

#include "Core/ModuleContext.hpp"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

#include <cstdint>

namespace
{

// `bLto` selects full optimisation rather than a genuine cross-module LTO run:
// one llvm::Module already holds the whole build (llvm.md's "one Module per
// build to start"), so there is nothing outside this module to link against yet.
// A later per-unit-module change is what would make this a real ThinLTO
// pipeline.
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

void Volt::Backend::Llvm::TargetPipeline::RunOptimizationPipeline () const
{
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB( Services->Ctx->MachinePtr() );

    PB.registerModuleAnalyses( MAM );
    PB.registerCGSCCAnalyses( CGAM );
    PB.registerFunctionAnalyses( FAM );
    PB.registerLoopAnalyses( LAM );
    PB.crossRegisterProxies( LAM, FAM, CGAM, MAM );

    const llvm::OptimizationLevel Level = OptimizationLevelOf( Services->Options->OptLevel, Services->Options->bLto );

    llvm::ModulePassManager MPM =
        Level == llvm::OptimizationLevel::O0 ? PB.buildO0DefaultPipeline( Level ) : PB.buildPerModuleDefaultPipeline( Level );
    MPM.run( Services->Ctx->Mod(), MAM );
}
