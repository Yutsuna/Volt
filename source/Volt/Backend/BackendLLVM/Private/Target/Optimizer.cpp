// Optimizer.cpp — PassBuilder's default pipelines, over a module first cut down
// to what the program can actually reach.
//
// Runs even at -O0: the O0 pipeline is the minimal semantically-required set,
// principally mem2reg, and this emitter never builds SSA itself (every alloca
// goes in the entry block) — it depends on that pass to promote them back to
// registers.
//
// The cut-down in front of it is the difference between a translation unit and
// a program. PassBuilder's pipelines are written for the former: a body with
// external linkage might be called by some *other* unit, so no amount of
// optimisation may delete it. This module is the latter — it holds `main`, so
// the set of entry points is closed and known — and saying so is what lets
// GlobalDCE remove a body nothing reaches. Without it every skipped-over stdlib
// declaration that did get a definition here (llvm.md's inline-eligible
// exception to the skip line) survives to the object file, and `--emit ir`
// prints the whole stdlib for a program that calls one function of it.

#include "Target/TargetPipeline.hpp"

#include "Volt/BackendCore/CompilerSeams.hpp"
#include "Volt/BackendCore/UnwindTransport.hpp"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/Internalize.h>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>

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

// Every symbol something outside this module still reaches by name, and the
// reason each one is not dead:
//
//   - the C entry symbol, because the runtime enters there;
//   - every compiler seam, because the precompiled stdlib artifact calls back
//     into them (`_V_init_all` and the rest are declared @[External( "volt" )]
//     by the prelude and left undefined in the artifact — its own copy would
//     have answered for a different build, so this build's definition is the
//     one that has to survive);
//   - the four unwind-transport slots, because they are *shared* state. Each is
//     a mergeable definition the linker folds with the artifact's own. Internalise
//     one and the two halves of the program each get a private copy of the
//     in-flight exception, so a raise inside the stdlib publishes into slots the
//     caller never reads — the failure would be silent, and only for exceptions
//     that cross the boundary.
//
// Derived, not spelled: the seams come out of the store the same way
// LinkerDriver reads @[External] libraries out of it, and the slot names come
// from the header that states them once for every backend
// (rules/zero-hardcode.md).
[[nodiscard]] std::unordered_set<std::string> WholeProgramExports ( const Volt::Backend::Llvm::AotServices &Services )
{
    std::unordered_set<std::string> Exports;

    Exports.emplace( Services.Options->EntrySymbol );

    Exports.emplace( Volt::Backend::UnwindTransport::ExceptionValueSlot );
    Exports.emplace( Volt::Backend::UnwindTransport::ExceptionTagSlot );
    Exports.emplace( Volt::Backend::UnwindTransport::BreakFlagSlot );
    Exports.emplace( Volt::Backend::UnwindTransport::ExceptionStorageSlot );
    Exports.emplace( Volt::Backend::UnwindTransport::SlotAccessorSymbol );

    if ( Services.Build == nullptr or Services.Build->Types == nullptr )
    {
        return Exports;
    }

    Volt::MiddleEnd::TypeSystem::TypeStore &Store = *Services.Build->Types;
    const auto Collect                            = [&] ( const Volt::MiddleEnd::TypeSystem::Member &Entry )
    {
        if ( not Entry.ExternLib.IsValid() or not Entry.ExternSymbol.IsValid() )
        {
            return;
        }
        if ( Store.Text( Entry.ExternLib ) == Volt::Backend::CompilerSeams::Library )
        {
            Exports.emplace( Store.Text( Entry.ExternSymbol ) );
        }
    };

    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        const Volt::MiddleEnd::TypeSystem::NominalId Id{
            static_cast<Volt::MiddleEnd::TypeSystem::NominalId::ValueType>( Index ) };
        for ( const Volt::MiddleEnd::TypeSystem::Member &Entry : Store.Type( Id ).Members )
        {
            Collect( Entry );
        }
    }
    for ( const Volt::MiddleEnd::TypeSystem::Member &Entry : Store.FreeFunctions() )
    {
        Collect( Entry );
    }

    return Exports;
}

} // namespace

void Volt::Backend::Llvm::TargetPipeline::RunOptimizationPipeline () const
{
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB( Services->Machine );

    PB.registerModuleAnalyses( MAM );
    PB.registerCGSCCAnalyses( CGAM );
    PB.registerFunctionAnalyses( FAM );
    PB.registerLoopAnalyses( LAM );
    PB.crossRegisterProxies( LAM, FAM, CGAM, MAM );

    const llvm::OptimizationLevel Level = OptimizationLevelOf( Services->Options->OptLevel, Services->Options->bLto );

    llvm::ModulePassManager MPM;

    // An entry symbol is what makes this module a program rather than a library:
    // a library build (the precompiled stdlib artifact, either kind) clears it
    // precisely because its callers are not in this build and its bodies are
    // therefore all live. `bSharedOutput` says the same thing a second way, and
    // the two are asked together so neither alone has to be the invariant.
    const bool bWholeProgram = not Services->Options->EntrySymbol.empty() and not Services->Options->bSharedOutput;

    if ( bWholeProgram )
    {
        // Before the pipeline, not after: internal linkage is an input to
        // inlining, argument promotion and constant propagation, so a body that
        // survives the cut is optimised knowing every one of its call sites.
        MPM.addPass( llvm::InternalizePass( [Exports = WholeProgramExports( *Services )] ( const llvm::GlobalValue &Global )
                                            { return Exports.contains( Global.getName().str() ); } ) );

        // Stated here rather than left to the pipeline: GlobalDCE is in the O2/O3
        // pipelines but not the O0 one, and at -O0 the dead bodies are exactly
        // the ones there is no point handing to the object emitter.
        MPM.addPass( llvm::GlobalDCEPass() );
    }

    MPM.addPass( Level == llvm::OptimizationLevel::O0 ? PB.buildO0DefaultPipeline( Level )
                                                      : PB.buildPerModuleDefaultPipeline( Level ) );
    MPM.run( *Services->Mod, MAM );
}
