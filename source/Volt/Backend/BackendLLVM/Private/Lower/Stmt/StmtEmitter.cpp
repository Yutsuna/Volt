// StmtEmitter.cpp — the one std::visit site for the statement category.

#include "Lower/Stmt/StmtEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/IRBuilder.h>

#include <string>
#include <variant>

void Volt::Backend::Llvm::BodyEmitter::EmitStmts ( const Frontend::StmtList &List, bool bTail )
{
    for ( std::size_t Index = 0; Index < List.Size(); ++Index )
    {
        if ( Failed() or Terminated() )
        {
            return;
        }
        EmitStmt( List[Index], bTail and Index + 1 == List.Size() );
    }
}

void Volt::Backend::Llvm::BodyEmitter::EmitStmt ( Frontend::StmtId Id, bool bTail )
{
    if ( Failed() or Frame().Unit == nullptr or not Id.IsValid() )
    {
        return;
    }

    const Frontend::AstContext &Ast = *Frame().Unit->Ast;

    std::visit(
        Meta::Overloaded{
            [this, bTail] ( const Frontend::ExprStmt &Node ) { EmitExprStmt( *this, Node, bTail ); },
            [this] ( const Frontend::While &Node ) { EmitWhile( *this, Node ); }, [this] ( const Frontend::Return &Node )
            { EmitReturn( *this, Node ); }, [this, Id] ( const Frontend::Break &Node ) { EmitBreak( *this, Id, Node ); },
            [this, Id] ( const Frontend::Next &Node ) { EmitNext( *this, Id, Node ); },
            [this, Id] ( const Frontend::LocalDecl &Node ) { EmitLocalDecl( *this, Id, Node ); },
            [this] ( const Frontend::RescueClause & )
            { static_cast<void>( Fail( "llvm: a `rescue` clause needs the exception emitter (backend phase 6)" ) ); },
            [this, Id] ( const auto & )
            {
                static_cast<void>( Fail( "llvm: " + std::string( Frontend::NodeName( Frame().Unit->Ast->Stmt( Id ) ) ) +
                                         " is not a statement this backend emits" ) );
            } },
        Ast.Stmt( Id ) );
}

void Volt::Backend::Llvm::EmitExprStmt ( BodyEmitter &Emitter, const Frontend::ExprStmt &Node, bool bTail )
{
    FunctionFrame &Frame = Emitter.Frame();

    // Outside result position the value is genuinely discarded — a call for its
    // effect.
    if ( not bTail or not Frame.bReturnsValue )
    {
        static_cast<void>( Emitter.EmitExpr( Node.Expr ) );
        return;
    }

    llvm::Value *Value = Emitter.EmitExpr( Node.Expr );
    if ( Value == nullptr )
    {
        return;
    }
    static_cast<void>( Emitter.Ctx().Builder().CreateRet( Emitter.CoerceWidth( Value, Frame.Fn->getReturnType() ) ) );
}
