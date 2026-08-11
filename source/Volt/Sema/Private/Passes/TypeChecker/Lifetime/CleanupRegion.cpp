#include "CleanupRegion.hpp"

#include "Volt/Frontend/AST/Expr.hpp"

#include <utility>

namespace Volt::Sema::TypeCheckerPass::Lifetime
{

namespace
{

    // The one place the boundary's current representation is spelled. Both
    // entry points below go through it, so `EmitSequence` cannot drift into a
    // different shape than `EmitBoundary` by accident.
    [[nodiscard]] Frontend::ExprId MakeBeginNode ( Frontend::AstContext &Ast,
                                                   UnitTypes &Values,
                                                   const Core::SourceRange Loc,
                                                   Frontend::StmtList Body,
                                                   Frontend::StmtList EnsureBody,
                                                   const SemaTypeId ResultType )
    {
        const Frontend::ExprId Id = Ast.Add( Frontend::ExprNode{ Frontend::BeginExpr{
            .Loc = Loc, .Body = std::move( Body ), .RescueClauses = {}, .EnsureBody = std::move( EnsureBody ) } } );
        if ( ResultType.IsValid() )
        {
            Values.SetExprType( Id, ResultType );
        }
        return Id;
    }

} // namespace

Frontend::ExprId EmitBoundary ( Frontend::AstContext &Ast,
                                UnitTypes &Values,
                                const Core::SourceRange Loc,
                                Frontend::StmtList Body,
                                Frontend::StmtList CleanupBody,
                                const SemaTypeId ResultType )
{
    return MakeBeginNode( Ast, Values, Loc, std::move( Body ), std::move( CleanupBody ), ResultType );
}

void EmitBoundaryInto ( Frontend::AstContext &Ast,
                        UnitTypes &Values,
                        const Frontend::ExprId Slot,
                        const Core::SourceRange Loc,
                        Frontend::StmtList Body,
                        Frontend::StmtList CleanupBody,
                        const SemaTypeId ResultType )
{
    // Built in a slot of its own first, then moved across: `MakeBeginNode`
    // calls `Add()`, which may reallocate the Expr arena, so a reference to
    // `Slot` taken before it would dangle (rules/ast-rewrite.md). The
    // assignment sequences the call before the destination is evaluated,
    // which is the same discipline every rewriting pass here follows.
    const Frontend::ExprId Built = MakeBeginNode( Ast, Values, Loc, std::move( Body ), std::move( CleanupBody ), ResultType );
    Ast.Expr( Slot )             = Ast.Expr( Built );
    if ( ResultType.IsValid() )
    {
        Values.SetExprType( Slot, ResultType );
    }
}

Frontend::ExprId EmitSequence ( Frontend::AstContext &Ast,
                                UnitTypes &Values,
                                const Core::SourceRange Loc,
                                Frontend::StmtList Body,
                                const SemaTypeId ResultType )
{
    return MakeBeginNode( Ast, Values, Loc, std::move( Body ), Frontend::StmtList{}, ResultType );
}

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
