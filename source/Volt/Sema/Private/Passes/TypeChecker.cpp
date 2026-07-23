// TypeChecker.cpp — Order 30 pass: gives every expression a type, and
// resolves the members that types make available.

#include "TypeChecker/DeclStmtWalker.hpp"
#include "TypeChecker/LiteralInferencer.hpp"
#include "TypeChecker/TypeCheckerContext.hpp"
#include "Volt/Sema/Pass.hpp"

void Volt::Sema::TypeChecker ( PassContext &Context )
{
    TypeCheckerPass::TypeCheckerContext State{ Context, TypeCheckerPass::MetadataExprs( Context.Ast ) };
    TypeCheckerPass::WalkDecls( State, Context.Ast.TopDecls );

    for ( const Frontend::StmtId Id : Context.Ast.TopStmts )
    {
        TypeCheckerPass::WalkStmt( State, Id );
    }
}
