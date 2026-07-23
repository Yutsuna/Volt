#include "ExprInferencer.hpp"

#include "ClosureInferencer.hpp"
#include "LiteralInferencer.hpp"
#include "MemberResolver.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::InferExpr ( TypeCheckerContext &Context, Frontend::ExprId Id )
{
    if ( not Id.IsValid() or ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] ) )
    {
        return SemaTypeId{};
    }
    if ( const SemaTypeId Known = Context.Ctx.Values.ExprType( Id ); Known.IsValid() )
    {
        return Known;
    }

    const SemaTypeId Type = ComputeExpr( Context, Id );
    Context.Ctx.Values.SetExprType( Id, Type );
    return Type;
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::ComputeExpr ( TypeCheckerContext &Context, Frontend::ExprId Id )
{
    const Frontend::ExprNode &Node = Context.Ctx.Ast.Expr( Id );

    return std::visit(
        Meta::Overloaded{
            [&] ( const Frontend::SelfExpr & ) -> SemaTypeId { return Context.SelfValue; },
            [&] ( const Frontend::InstanceVar &Expr ) -> SemaTypeId
            {
                return MemberType( Context, Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ), Context.SelfValue,
                                   Context.bStaticContext, Context.Ctx.Ast.Text( Expr.Name ) );
            },
            [&] ( const Frontend::Identifier &Expr ) -> SemaTypeId
            {
                if ( const std::optional<SemaTypeId> Local = Context.FindLocal( Id, Expr.Name ) )
                {
                    return *Local;
                }
                if ( const auto Named = Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Expr.Name ) ) )
                {
                    Context.NakedTypeExprs.insert( Id.Value );
                    return Context.MakeType( *Named, {} );
                }
                return SemaTypeId{};
            },
            [&] ( const Frontend::Member &Expr ) -> SemaTypeId
            {
                const SemaTypeId Object            = InferExpr( Context, Expr.Object );
                const Resolution Found             = LookupOn( Context, Object, Context.Ctx.Ast.Text( Expr.Name ) );
                Context.CalleeResolution[Id.Value] = Found;
                if ( Context.Ctx.Values.Has( Object ) and Found.Decl == nullptr )
                {
                    Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ),
                                    "type " + Context.NameOfValue( Object ) + " has no member '" +
                                        std::string{ Context.Ctx.Ast.Text( Expr.Name ) } + "'" );
                }
                CheckMemberSelf( Context, Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ), Found,
                                 Context.NakedTypeExprs.contains( Expr.Object.Value ) );
                return Found.Result;
            },
            [&] ( const Frontend::Index &Expr ) -> SemaTypeId
            {
                const SemaTypeId Object = InferExpr( Context, Expr.Object );
                for ( const Frontend::ExprId Arg : Expr.Args )
                {
                    static_cast<void>( InferExpr( Context, Arg ) );
                }
                return MemberType( Context, Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ), Object,
                                   Context.NakedTypeExprs.contains( Expr.Object.Value ), IndexOperator );
            },
            [&] ( const Frontend::Binary &Expr ) -> SemaTypeId
            {
                const SemaTypeId Lhs = InferExpr( Context, Expr.Lhs );
                const SemaTypeId Rhs = InferExpr( Context, Expr.Rhs );
                if ( Lhs.IsValid() and not Context.UnconstrainedLiterals.contains( Expr.Lhs.Value ) )
                {
                    Context.ConstrainExprType( Expr.Rhs, Lhs );
                }
                else if ( Rhs.IsValid() and not Context.UnconstrainedLiterals.contains( Expr.Rhs.Value ) )
                {
                    Context.ConstrainExprType( Expr.Lhs, Rhs );
                }
                return MemberType( Context, Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ), InferExpr( Context, Expr.Lhs ),
                                   Context.NakedTypeExprs.contains( Expr.Lhs.Value ), Frontend::TokenSpelling( Expr.Op ) );
            },
            [&] ( const Frontend::Unary &Expr ) -> SemaTypeId
            {
                return MemberType( Context, Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ), InferExpr( Context, Expr.Operand ),
                                   Context.NakedTypeExprs.contains( Expr.Operand.Value ), Frontend::TokenSpelling( Expr.Op ) );
            },
            [&] ( const Frontend::Assign &Expr ) -> SemaTypeId
            {
                const Volt::Sema::SemaTypeId Value = InferExpr( Context, Expr.Value );
                if ( const auto *Target = std::get_if<Frontend::Identifier>( &Context.Ctx.Ast.Expr( Expr.Target ) ) )
                {
                    const std::optional<SemaTypeId> Known = Context.FindLocal( Expr.Target, Target->Name );
                    if ( Known.has_value() and Known->IsValid() )
                    {
                        Context.ConstrainExprType( Expr.Value, *Known );
                    }
                    else
                    {
                        Context.WriteLocal( Expr.Target, Target->Name, Value );
                        Context.UnconstrainedVarInitializers[Target->Name] = Expr.Value;
                    }
                }
                static_cast<void>( InferExpr( Context, Expr.Target ) );
                return Value;
            },
            [&] ( const Frontend::Ternary &Expr ) -> SemaTypeId
            {
                static_cast<void>( InferExpr( Context, Expr.Cond ) );
                const SemaTypeId Then = InferExpr( Context, Expr.Then );
                const SemaTypeId Else = InferExpr( Context, Expr.Else );
                return Then.IsValid() ? Then : Else;
            },
            [&] ( const Frontend::Call &Expr ) -> SemaTypeId { return CallType( Context, Expr ); },
            [&] ( const Frontend::GenericInst &Expr ) -> SemaTypeId { return GenericInstType( Context, Id, Expr ); },
            [&] ( const Frontend::DotCall &Expr ) -> SemaTypeId
            {
                for ( const Frontend::ExprId Arg : Expr.Args )
                {
                    static_cast<void>( InferExpr( Context, Arg ) );
                }
                const Resolution Found = LookupOn( Context, Context.SelfValue, Context.Ctx.Ast.Text( Expr.Method ) );
                if ( Context.Ctx.Values.Has( Context.SelfValue ) and Found.Decl == nullptr )
                {
                    Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ),
                                    "type " + Context.NameOfValue( Context.SelfValue ) + " has no member '" +
                                        std::string{ Context.Ctx.Ast.Text( Expr.Method ) } + "'" );
                }
                CheckDotCallSelf( Context, Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ), Found );
                CheckCallArgs( Context, Expr.Loc, Found, Expr.Args );
                return Found.Result;
            },
            [&] ( const Frontend::Lambda &Expr ) -> SemaTypeId
            {
                const Core::SmallVec<SemaTypeId, 2> ParamTypes = BindClosureParams( Context, Expr.Params );
                if ( Expr.ReturnType.IsValid() )
                {
                    const SemaTypeId WrittenRet = InferExpr( Context, Expr.ReturnType );
                    if ( WrittenRet.IsValid() )
                    {
                        Context.ConstrainExprType( Expr.Body, WrittenRet );
                    }
                }
                return ClosureType( Context, "Lambda", InferExpr( Context, Expr.Body ), ParamTypes );
            },
            [&] ( const Frontend::Block &Expr ) -> SemaTypeId
            {
                const Core::SmallVec<SemaTypeId, 2> ParamTypes = BindClosureParams( Context, Expr.Params );
                return ClosureType( Context, "Block", TrailingType( Context, Expr.Body ), ParamTypes );
            },
            [&] ( const auto &Expr ) -> SemaTypeId { return LiteralType( Context, Id, Expr ); },
        },
        Node );
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::CallType ( TypeCheckerContext &Context, const Frontend::Call &Expr )
{
    for ( const Frontend::ExprId Arg : Expr.Args )
    {
        static_cast<void>( InferExpr( Context, Arg ) );
    }
    static_cast<void>( InferExpr( Context, Expr.BlockArg ) );

    const SemaTypeId Result = InferExpr( Context, Expr.Callee );

    if ( const auto It = Context.CalleeResolution.find( Expr.Callee.Value ); It != Context.CalleeResolution.end() )
    {
        CheckCallArgs( Context, Expr.Loc, It->second, Expr.Args );
    }

    return Result;
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::GenericInstType ( TypeCheckerContext &Context,
                                                                      Frontend::ExprId Id,
                                                                      const Frontend::GenericInst &Expr )
{
    const SemaTypeId Base = InferExpr( Context, Expr.Base );
    if ( Context.NakedTypeExprs.contains( Expr.Base.Value ) )
    {
        Context.NakedTypeExprs.insert( Id.Value );
    }
    if ( not Context.Ctx.Values.Has( Base ) )
    {
        return SemaTypeId{};
    }
    const NominalId Nominal = Context.Ctx.Values.Get( Base ).Base;

    Core::SmallVec<SemaTypeId, 2> Args;
    for ( const Frontend::TypeId Arg : Expr.Args )
    {
        UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Context.SelfValue };
        Args.PushBack( ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Arg ) );
    }

    CheckArity( Context, Expr.Loc, Nominal, Args.Size() );
    return Context.MakeType( Nominal, std::move( Args ) );
}
