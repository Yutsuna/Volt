// OptimizationLevel.cpp — see OptimizationLevel.hpp.

#include "Volt/BackendLlvmIr/OptimizationLevel.hpp"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

#include <map>
#include <utility>
#include <vector>

llvm::OptimizationLevel Volt::Backend::Ir::OptimizationLevelOf ( const std::uint8_t OptLevel )
{
    if ( OptLevel >= 3 )
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

void Volt::Backend::Ir::RunOptimizationPipeline ( llvm::Module &Mod,
                                                  const llvm::OptimizationLevel Level,
                                                  llvm::TargetMachine *const Machine )
{
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB( Machine );
    PB.registerModuleAnalyses( MAM );
    PB.registerCGSCCAnalyses( CGAM );
    PB.registerFunctionAnalyses( FAM );
    PB.registerLoopAnalyses( LAM );
    PB.crossRegisterProxies( LAM, FAM, CGAM, MAM );

    llvm::ModulePassManager MPM =
        Level == llvm::OptimizationLevel::O0 ? PB.buildO0DefaultPipeline( Level ) : PB.buildPerModuleDefaultPipeline( Level );
    MPM.run( Mod, MAM );
}

// -Wnull-dereference is on for the whole project on purpose. At -O3 GCC inlines
// LLVM intrusive-list iterators / accessors (ilist_node_base.h) and falsely
// warns on list sentinels. Scoped locally to avoid suppressing real warnings elsewhere.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"

bool Volt::Backend::Ir::HasLoop ( const llvm::Function &Fn )
{
    if ( Fn.empty() )
    {
        return false;
    }
    if ( Fn.size() == 1 )
    {
        const llvm::BasicBlock &Entry = Fn.getEntryBlock();
        const llvm::Instruction *Term = Entry.getTerminator();
        if ( Term == nullptr )
        {
            return false;
        }
        for ( unsigned I = 0; I < Term->getNumSuccessors(); ++I )
        {
            if ( Term->getSuccessor( I ) == &Entry )
            {
                return true;
            }
        }
        return false;
    }

    // Standard DFS back-edge detection: 0 = unvisited, 1 = visiting (in stack), 2 = visited
    std::map<const llvm::BasicBlock *, std::uint8_t> State;
    struct DfsFrame
    {
        const llvm::BasicBlock *BB = nullptr;
        unsigned SuccIndex         = 0;
    };
    std::vector<DfsFrame> DfsStack;

    const llvm::BasicBlock *Entry = &Fn.getEntryBlock();
    State[Entry]                  = 1;
    DfsStack.push_back( DfsFrame{ .BB = Entry, .SuccIndex = 0 } );

    while ( not DfsStack.empty() )
    {
        DfsFrame &Frame               = DfsStack.back();
        const llvm::Instruction *Term = Frame.BB->getTerminator();
        const unsigned NumSuccs       = Term != nullptr ? Term->getNumSuccessors() : 0;

        if ( Frame.SuccIndex >= NumSuccs )
        {
            State[Frame.BB] = 2;
            DfsStack.pop_back();
            continue;
        }

        const llvm::BasicBlock *Succ = Term->getSuccessor( Frame.SuccIndex++ );
        const auto [It, Inserted]    = State.try_emplace( Succ, std::uint8_t{ 0 } );
        if ( It->second == 1 )
        {
            return true; // Back-edge found -> loop present
        }
        if ( It->second == 0 )
        {
            It->second = 1;
            DfsStack.push_back( DfsFrame{ .BB = Succ, .SuccIndex = 0 } );
        }
    }
    return false;
}

bool Volt::Backend::Ir::IsCandidateForOptimization ( const llvm::Function &Fn )
{
    if ( Fn.isDeclaration() )
    {
        return false;
    }
    if ( HasLoop( Fn ) )
    {
        return true;
    }
    // Substantial straight-line body (> 30 instructions)
    std::size_t InstCount = 0;
    for ( const llvm::BasicBlock &BB : Fn )
    {
        InstCount += BB.size();
        if ( InstCount >= 30 )
        {
            return true;
        }
    }
    return false;
}

#pragma GCC diagnostic pop

