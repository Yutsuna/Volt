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

Frontend::ExprId EmitSequence ( Frontend::AstContext &Ast,
                                UnitTypes &Values,
                                const Core::SourceRange Loc,
                                Frontend::StmtList Body,
                                const SemaTypeId ResultType )
{
    return MakeBeginNode( Ast, Values, Loc, std::move( Body ), Frontend::StmtList{}, ResultType );
}

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
