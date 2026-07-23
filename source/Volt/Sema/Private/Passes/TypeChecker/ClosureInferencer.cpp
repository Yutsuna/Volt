
#include "ClosureInferencer.hpp"

#include "DeclStmtWalker.hpp"
#include "ExprInferencer.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

Volt::Core::SmallVec<Volt::Sema::SemaTypeId, 2>
Volt::Sema::TypeCheckerPass::BindClosureParams ( TypeCheckerContext &Context, const Frontend::ParamList &Params )
{
    UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Context.SelfValue };
    Core::SmallVec<SemaTypeId, 2> Types;
    for ( const Frontend::ParamId PId : Params )
    {
        const Frontend::Param &Entry = Context.Ctx.Ast.GetParam( PId );
        const SemaTypeId ParamType =
            ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Entry.DeclType );
        Context.LocalTypes[BindingSite{ PId }] = ParamType;
        Context.Locals[Entry.Name]             = ParamType;
        Context.Ctx.Values.SetSiteType( BindingSite{ PId }, ParamType );
        if ( Entry.Default.IsValid() )
        {
            static_cast<void>( InferExpr( Context, Entry.Default ) );
        }
        Types.PushBack( ParamType );
    }
    return Types;
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::TrailingType ( TypeCheckerContext &Context, const Frontend::StmtList &Body )
{
    SemaTypeId Last;
    for ( const Frontend::StmtId Id : Body )
    {
        WalkStmt( Context, Id );
        Last = SemaTypeId{};
        if ( const auto *Stmt = std::get_if<Frontend::ExprStmt>( &Context.Ctx.Ast.Stmt( Id ) ) )
        {
            Last = Context.Ctx.Values.ExprType( Stmt->Expr );
        }
    }
    return Last;
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::ClosureType ( TypeCheckerContext &Context,
                                                                  std::string_view NodeKind,
                                                                  Volt::Sema::SemaTypeId Result,
                                                                  const Volt::Core::SmallVec<Volt::Sema::SemaTypeId, 2> &Params )
{
    const auto Base = Context.Ctx.Types.LookupNodeKind( NodeKind );
    if ( not Base )
    {
        return SemaTypeId{};
    }

    Core::SmallVec<SemaTypeId, 2> Args;
    Args.PushBack( Result );
    for ( const SemaTypeId Param : Params )
    {
        Args.PushBack( Param );
    }
    return Context.MakeType( *Base, std::move( Args ) );
}
