// ExprShortCircuit.cpp — `and` / `or` (and `&&` / `||`).
//
// Spelled operators that short-circuit, so they are control flow and never a
// table row: the right-hand side is evaluated in its own block, reached only on
// the outcome that needs it.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include "Volt/Frontend/Lexer/Token.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>

// NOLINTBEGIN(clang-analyzer-security.ArrayBound) — false positive via LLVM's hung-off operand layout
llvm::Value *Volt::Backend::Llvm::EmitShortCircuit ( BodyEmitter &Emitter, const Frontend::Binary &Node )
{
    const bool bAnd = Node.Op == Frontend::TokenKind::KwAnd or Node.Op == Frontend::TokenKind::AndAnd;

    llvm::Value *Lhs = Emitter.EmitExpr( Node.Lhs );
    if ( Lhs == nullptr )
    {
        return nullptr;
    }

    llvm::LLVMContext &Context = Emitter.Ctx().Context();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();
    llvm::Function *Fn         = Emitter.Frame().Fn;

    llvm::BasicBlock *Origin = Builder.GetInsertBlock();
    llvm::BasicBlock *Rest   = llvm::BasicBlock::Create( Context, bAnd ? "and.rhs" : "or.rhs", Fn );
    llvm::BasicBlock *Merge  = llvm::BasicBlock::Create( Context, bAnd ? "and.end" : "or.end", Fn );

    // `a and b` evaluates b only when a held; `a or b` only when it did not.
    static_cast<void>( bAnd ? Builder.CreateCondBr( Lhs, Rest, Merge ) : Builder.CreateCondBr( Lhs, Merge, Rest ) );

    Builder.SetInsertPoint( Rest );
    llvm::Value *Rhs = Emitter.EmitExpr( Node.Rhs );
    if ( Rhs == nullptr )
    {
        return nullptr;
    }
    llvm::BasicBlock *RhsEnd = Builder.GetInsertBlock();
    static_cast<void>( Builder.CreateBr( Merge ) );

    Builder.SetInsertPoint( Merge );
    llvm::PHINode *Result = Builder.CreatePHI( Lhs->getType(), 2, "logic" );
    Result->addIncoming( bAnd ? Builder.getFalse() : Builder.getTrue(), Origin );
    Result->addIncoming( Rhs, RhsEnd );
    return Result;
}
// NOLINTEND(clang-analyzer-security.ArrayBound)
