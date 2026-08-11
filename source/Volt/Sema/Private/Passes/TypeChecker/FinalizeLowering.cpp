#include "FinalizeLowering.hpp"

#include "Lifetime/FieldCascade.hpp"
#include "Lifetime/ScopeCleanup.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"

#include <cstddef>
#include <utility>
#include <variant>

// The RAII sweep's orchestrator. It owns no analysis of its own: it decides
// *what gets a region*, and delegates everything else.
//
//   Lifetime/FieldCascade    — a type finalizes what its fields own
//   Lifetime/ScopeCleanup    — a body finalizes what its locals own
//   Lifetime/CleanupRegion   — the one cleanup boundary a region lowers to
//   Lifetime/ExitPaths       — which paths leave a region, and what they carry
//   Lifetime/FinalizeCallBuilder — the call tree a release lowers to
//   Raii/Ownership           — is this type finalizable, and what does it hold
void Volt::Sema::TypeCheckerPass::InsertFinalizeCalls ( TypeCheckerContext &Context )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Runs first, so a type's own `finalize` already carries its field
    // cascade by the time the per-Method sweep below builds regions around
    // it — the cascade then rides the ordinary tail like hand-written code.
    Lifetime::RunFieldCascade( Context );

    // A file's own top-level statements are a distinct list from any Method
    // body (Ast.TopStmts, a plain std::vector<StmtId> — never a
    // Frontend::StmtList — walked under the Unit root Scope ScopeResolver
    // pushes once per file, ScopeResolver.cpp's own `WalkStmt( Id, Root )`
    // loop). A Decl-only loop silently skips every top-level `begin/rescue`
    // or `Array<T>.new(...)` local — exactly the shape RaiseUnwind.vl's own
    // top-level `begin ... rescue e ... end` uses. Converted to a StmtList
    // and back, since that is what a region is defined over.
    if ( not Ast.TopStmts.empty() )
    {
        Frontend::StmtList TopBody;
        for ( const Frontend::StmtId Id : Ast.TopStmts )
        {
            TopBody.PushBack( Id );
        }

        if ( Lifetime::RunScopeCleanup( Context, Context.Ctx.Scopes.ScopeOf( Ast.TopStmts[0] ), TopBody ) )
        {
            Ast.TopStmts.clear();
            for ( const Frontend::StmtId Id : TopBody )
            {
                Ast.TopStmts.push_back( Id );
            }
        }
    }

    // Decls never grow in this pass (only a Method's own Body slot is
    // rewritten, in place), but the count is still snapshotted before the
    // first Add() per rules/ast-rewrite.md's discipline.
    const std::size_t OriginalDeclCount = Ast.DeclCount();
    for ( std::size_t Index = 0; Index < OriginalDeclCount; ++Index )
    {
        const Frontend::DeclId Id{ static_cast<Frontend::DeclId::ValueType>( Index ) };
        if ( not std::holds_alternative<Frontend::Method>( Ast.Decl( Id ) ) )
        {
            continue;
        }

        // Copied out before any Add() below appends to the Expr/Stmt arenas
        // (rules/ast-rewrite.md) — Node is a value from here on.
        Frontend::Method Node = std::get<Frontend::Method>( Ast.Decl( Id ) );

        if ( Node.bAbstract or Node.bExternal or Node.Body.IsEmpty() )
        {
            continue;
        }

        if ( Lifetime::RunScopeCleanup( Context, Context.Ctx.Scopes.ScopeOf( Node.Body[0] ), Node.Body ) )
        {
            Ast.Decl( Id ) = Frontend::DeclNode{ std::move( Node ) };
        }
    }
}
