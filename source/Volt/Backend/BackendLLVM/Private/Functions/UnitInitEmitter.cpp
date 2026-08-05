// UnitInitEmitter.cpp — `_V_init_<Ordinal>`, a unit's top-level statements.
//
// Unlike the entry shim, this one *is* emitted from Volt source (the unit's own
// TopStmts, in file order), so it goes through an ordinary BodyEmitter over a
// frame of its own — a top-level `x = 1` mints a module global through the same
// SlotFor path any other binding uses.

#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <string>

void Volt::Backend::Llvm::EmitUnitInit ( EmitterServices &Services, const UnitView &Unit )
{
    if ( Unit.Ast == nullptr )
    {
        return;
    }

    llvm::Module &Mod = Services.Ctx->Mod();

    const std::string InitName = "_V_init_" + std::to_string( Unit.Ordinal );
    llvm::Function *InitFn     = Mod.getFunction( InitName );
    if ( InitFn == nullptr )
    {
        llvm::FunctionType *FnTy = llvm::FunctionType::get( Services.Ctx->Builder().getVoidTy(), false );
        InitFn                   = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, InitName, &Mod );
    }

    FunctionFrame Frame;
    Frame.Fn            = InitFn;
    Frame.Unit          = &Unit;
    Frame.Values        = Unit.Values;
    Frame.Callees       = Unit.Callees;
    Frame.Entry         = llvm::BasicBlock::Create( Services.Ctx->Context(), "entry", InitFn );
    Frame.bReturnsValue = false;

    BodyEmitter Emitter( Services, Frame );
    llvm::IRBuilder<> &Builder = Services.Ctx->Builder();
    Builder.SetInsertPoint( Frame.Entry );

    for ( const Frontend::StmtId StmtId : Unit.Ast->TopStmts )
    {
        Emitter.EmitStmt( StmtId, /*bTail=*/false );
    }

    if ( not Emitter.Terminated() )
    {
        static_cast<void>( Builder.CreateRetVoid() );
    }
}
