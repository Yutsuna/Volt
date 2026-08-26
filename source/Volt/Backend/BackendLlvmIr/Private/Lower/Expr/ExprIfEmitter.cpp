// ExprIfEmitter.cpp — `if` in value position.
//
// An `elsif` chain is a nested If inside Else (Expr.hpp), so it is emitted by
// the recursive call the else branch makes through EmitStmt — the chain needs no
// special case of its own.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>

llvm::Value *Volt::Backend::Llvm::EmitIf ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::If &Node )
{
    llvm::Value *Cond = Emitter.EmitExpr( Node.Cond );
    if ( Cond == nullptr )
    {
        return nullptr;
    }

    llvm::LLVMContext &Context = Emitter.Ctx().Context();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();
    llvm::Function *Fn         = Emitter.Frame().Fn;

    const bool bHasElse = Node.Else.Size() > 0;

    llvm::BasicBlock *Then  = llvm::BasicBlock::Create( Context, "if.then", Fn );
    llvm::BasicBlock *Else  = bHasElse ? llvm::BasicBlock::Create( Context, "if.else", Fn ) : nullptr;
    llvm::BasicBlock *Merge = llvm::BasicBlock::Create( Context, "if.end", Fn );
    static_cast<void>( Builder.CreateCondBr( Cond, Then, bHasElse ? Else : Merge ) );

    // No shape means the `if` is in statement position (or a `-> Void` tail):
    // there is nothing to converge, and the branches are emitted for effect.
    llvm::Type *Shape                            = Emitter.TypeOfExpr( Id );
    const MiddleEnd::TypeSystem::LayoutId Layout = Emitter.LayoutOfExpr( Id );
    llvm::AllocaInst *Slot                       = Shape != nullptr ? Emitter.MakeTemp( Shape, "if.result" ) : nullptr;

    Builder.SetInsertPoint( Then );
    EmitConvergingBody( Emitter, Node.Then, Slot, Shape, Layout, Merge );

    if ( bHasElse )
    {
        Builder.SetInsertPoint( Else );
        EmitConvergingBody( Emitter, Node.Else, Slot, Shape, Layout, Merge );
    }

    Builder.SetInsertPoint( Merge );
    if ( Slot == nullptr )
    {
        return nullptr;
    }
    return Emitter.LoadConverged( Slot, Shape, Layout, "if" );
}
