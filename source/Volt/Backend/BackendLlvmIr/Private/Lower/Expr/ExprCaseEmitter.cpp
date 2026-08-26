// ExprCaseEmitter.cpp — `case` in value position.
//
// After CaseLowering (order 22) this is a flat list of
// `WhenClause{ Patterns: [expr Bool], Body }` — the pattern *is* the test, and
// the scrutinee has already been folded into it. So the emission is a
// conditional ladder and nothing more; that flatness is exactly why CaseExpr
// stayed core (rules/core-ast.md).

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>

#include <variant>

llvm::Value *Volt::Backend::Llvm::EmitCase ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::CaseExpr &Node )
{
    if ( Node.Target.IsValid() )
    {
        static_cast<void>( Emitter.Fail( "llvm: a `case` still carrying its target reached codegen — CaseLowering folds it "
                                         "into each clause's patterns" ) );
        return nullptr;
    }

    const Frontend::AstContext &Ast = *Emitter.Frame().Unit->Ast;
    llvm::LLVMContext &Context      = Emitter.Ctx().Context();
    llvm::IRBuilder<> &Builder      = Emitter.Ctx().Builder();
    llvm::Function *Fn              = Emitter.Frame().Fn;

    llvm::BasicBlock *Merge = llvm::BasicBlock::Create( Context, "case.end", Fn );

    // A `case` in value position needs a slot: its clauses are statement lists,
    // so their results converge through storage rather than a phi over blocks
    // whose count is not known until the ladder is built. An aggregate result
    // gets a slot too — `StoreTailValue` converges it by `EmitStore`'s memcpy,
    // not a plain `CreateStore` of an SSA struct value.
    llvm::Type *Shape                            = Emitter.TypeOfExpr( Id );
    const MiddleEnd::TypeSystem::LayoutId Layout = Emitter.LayoutOfExpr( Id );
    llvm::AllocaInst *Slot                       = nullptr;
    if ( Shape != nullptr )
    {
        Slot = Emitter.MakeTemp( Shape, "case.result" );
    }

    for ( const Frontend::StmtId ClauseId : Node.Clauses )
    {
        const auto *Clause = std::get_if<Frontend::WhenClause>( &Ast.Stmt( ClauseId ) );
        if ( Clause == nullptr )
        {
            static_cast<void>( Emitter.Fail( "llvm: a `case` clause is not a WhenClause" ) );
            return nullptr;
        }

        llvm::BasicBlock *Body = llvm::BasicBlock::Create( Context, "when.body", Fn );
        llvm::BasicBlock *Next = llvm::BasicBlock::Create( Context, "when.next", Fn );

        // Several patterns on one clause are alternatives: any match enters the
        // body, so each falls through to the next test rather than to `Next`.
        for ( std::size_t Index = 0; Index < Clause->Patterns.Size(); ++Index )
        {
            llvm::Value *Test = Emitter.EmitExpr( Clause->Patterns[Index] );
            if ( Test == nullptr )
            {
                return nullptr;
            }
            const bool bLast         = Index + 1 == Clause->Patterns.Size();
            llvm::BasicBlock *Missed = bLast ? Next : llvm::BasicBlock::Create( Context, "when.alt", Fn );
            static_cast<void>( Builder.CreateCondBr( Test, Body, Missed ) );
            Builder.SetInsertPoint( Missed );
        }
        if ( Clause->Patterns.Size() == 0 )
        {
            static_cast<void>( Builder.CreateBr( Next ) );
            Builder.SetInsertPoint( Next );
        }

        llvm::BasicBlock *Resume = Builder.GetInsertBlock();
        Builder.SetInsertPoint( Body );
        EmitConvergingBody( Emitter, Clause->Body, Slot, Shape, Layout, Merge );
        Builder.SetInsertPoint( Resume );

        if ( Emitter.Failed() )
        {
            return nullptr;
        }
    }

    EmitConvergingBody( Emitter, Node.ElseBody, Slot, Shape, Layout, Merge );

    Builder.SetInsertPoint( Merge );
    if ( Slot == nullptr )
    {
        return nullptr;
    }
    return Emitter.LoadConverged( Slot, Shape, Layout, "case" );
}
