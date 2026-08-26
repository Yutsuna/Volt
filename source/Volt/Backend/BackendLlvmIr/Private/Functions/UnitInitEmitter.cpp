// UnitInitEmitter.cpp — `_V_init_<Ordinal>` and `_V_fini_<Ordinal>`: a unit's
// top-level statements, and what runs when it is torn down.
//
// Unlike the entry shim, this one *is* emitted from Volt source (the unit's own
// TopStmts, in file order), so it goes through an ordinary BodyEmitter over a
// frame of its own — a top-level `x = 1` mints a module global through the same
// SlotFor path any other binding uses.

#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include "Volt/BackendCore/InitAllSynthesizer.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <string>
#include <vector>

namespace
{

// Both functions have the same shape — a void, no-argument function whose body
// is a StmtList of the unit's own — so they are emitted by one helper. Only
// which list, and under which name, differs.
void EmitUnitBody ( Volt::Backend::Llvm::EmitterServices &Services,
                    const Volt::Backend::UnitView &Unit,
                    const std::string &Name,
                    const std::vector<Volt::Frontend::StmtId> &Body )
{
    using namespace Volt::Backend::Llvm;

    llvm::Module &Mod  = Services.Ctx->Mod();
    llvm::Function *Fn = Mod.getFunction( Name );
    if ( Fn == nullptr )
    {
        llvm::FunctionType *FnTy = llvm::FunctionType::get( Services.Ctx->Builder().getVoidTy(), false );
        Fn                       = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, Name, &Mod );
    }

    FunctionFrame Frame;
    Frame.Fn            = Fn;
    Frame.Unit          = &Unit;
    Frame.Values        = Unit.Values;
    Frame.Callees       = Unit.Callees;
    Frame.Entry         = llvm::BasicBlock::Create( Services.Ctx->Context(), "entry", Fn );
    Frame.bReturnsValue = false;

    BodyEmitter Emitter( Services, Frame );
    llvm::IRBuilder<> &Builder = Services.Ctx->Builder();
    Builder.SetInsertPoint( Frame.Entry );

    for ( const Volt::Frontend::StmtId Id : Body )
    {
        Emitter.EmitStmt( Id, /*bTail=*/false );
    }

    if ( not Emitter.Terminated() )
    {
        static_cast<void>( Builder.CreateRetVoid() );
    }
}

} // namespace

void Volt::Backend::Llvm::EmitUnitFini ( EmitterServices &Services, const UnitView &Unit )
{
    // Emitted only when there is something to release. A unit with no module
    // variable of its own has no `_V_fini_<N>`, and SynthesizeFiniAll leaves it
    // out of the teardown sequence for the same reason.
    if ( not Backend::UnitHasFini( Unit ) )
    {
        return;
    }
    EmitUnitBody( Services, Unit, "_V_fini_" + std::to_string( Unit.Ordinal ), Unit.Ast->TopTeardown );
}

void Volt::Backend::Llvm::EmitUnitInit ( EmitterServices &Services, const UnitView &Unit )
{
    // Emitted only when there is something to run, the mirror of EmitUnitFini
    // above: this body is the unit's top-level statements and nothing else, so
    // an empty top level is an empty function, and SynthesizeInitAll leaves it
    // out of the init sequence for the same reason.
    if ( not Backend::UnitHasInit( Unit ) )
    {
        return;
    }
    EmitUnitBody( Services, Unit, "_V_init_" + std::to_string( Unit.Ordinal ), Unit.Ast->TopStmts );
}
