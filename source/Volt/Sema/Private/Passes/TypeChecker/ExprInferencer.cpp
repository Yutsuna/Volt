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

namespace
{

// A closure type's parameter slots are fully inferred once every entry past
// `Args[0]` (the `Lambda`/`Block` nominal's Result, see ClosureType) carries
// a valid SemaTypeId. Result is excluded: a block whose body is all
// statements — `each do |item| ... end` — never types a trailing expression,
// so an invalid Result there is the ordinary Void case, not a failure to
// infer. A point-free value passed as `&block` has nothing but the call site
// to take a *parameter* type from — if that call site cannot supply one
// either (an unannotated local, not a literal block under an
// `ExpectedClosure` slot), the slot stays open and must be reported, not
// silently handed through as `Array<?>`.
[[nodiscard]] bool IsBlockFullyInferred ( const Volt::Sema::TypeCheckerPass::TypeCheckerContext &Context,
                                          Volt::Sema::SemaTypeId BlockType )
{
    if ( not BlockType.IsValid() or not Context.Ctx.Values.Has( BlockType ) )
    {
        return false;
    }
    const auto &Slots = Context.Ctx.Values.Get( BlockType ).Args;
    for ( std::size_t Index = 1; Index < Slots.Size(); ++Index )
    {
        if ( not Slots[Index].IsValid() )
        {
            return false;
        }
    }
    return true;
}

} // namespace

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

                // Neither a local nor a type: inside a method body, a bare
                // name is a member of `self`. Resolving it here is what makes
                // `each do | item | ... end` behave like `self.each do ... end`
                // — same CalleeResolution, so the block gets its parameter
                // types and the method generics still infer. Staying silent
                // when nothing matches is deliberate: a free function call
                // lands here too, and it is not this branch's business.
                if ( Context.SelfValue.IsValid() )
                {
                    const Resolution Found = LookupOn( Context, Context.SelfValue, Context.Ctx.Ast.Text( Expr.Name ) );
                    if ( Found.Decl != nullptr )
                    {
                        Context.CalleeResolution[Id.Value] = Found;
                        return Found.Result;
                    }
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
    // The callee comes first: resolving it is what fills CalleeResolution,
    // and it depends on neither the arguments nor the block. Only once it is
    // known can the trailing `do ... end` be typed against the `&block` slot
    // the callee declares — inferring the block first, as this used to, left
    // its parameters with nothing to take a type from.
    const SemaTypeId Callee = InferExpr( Context, Expr.Callee );

    // A callee that resolved to no member may still be callable: a local
    // holding a closure is called as `f( x )`, which means the member its
    // type annotates `@[Apply]`. Without this, `f( x )` evaluated to `f`
    // itself, and every point-free pipeline stayed typed as its own Proc.
    auto It = Context.CalleeResolution.find( Expr.Callee.Value );
    if ( It == Context.CalleeResolution.end() )
    {
        const Resolution Applied = LookupApplyOn( Context, Callee );
        if ( Applied.Decl != nullptr )
        {
            It = Context.CalleeResolution.emplace( Expr.Callee.Value, Applied ).first;
        }
    }

    if ( It == Context.CalleeResolution.end() )
    {
        for ( const Frontend::ExprId Arg : Expr.Args )
        {
            static_cast<void>( InferExpr( Context, Arg ) );
        }
        static_cast<void>( InferExpr( Context, Expr.BlockArg ) );
        return Callee;
    }

    Resolution &Found = It->second;

    // Arguments are bound before being checked: a method generic appearing
    // in a parameter has to learn its type from the actual argument first,
    // or every call to `def id<U>( x : U )` would be reported as a mismatch
    // against a slot nothing had filled.
    for ( const Frontend::ExprId Arg : Expr.Args )
    {
        static_cast<void>( InferExpr( Context, Arg ) );
    }
    UnifyArgs( Context, Found, Expr.Args );
    CheckCallArgs( Context, Expr.Loc, Found, Expr.Args );

    if ( Expr.BlockArg.IsValid() )
    {
        // The block is typed *under* the slot the callee declares — that is
        // what gives `| i |` a type — and then answers back: what the block
        // returns is what binds the `U` of `def map<U>( &block : T -> U )`.
        const SemaTypeId OuterExpected = Context.ExpectedClosure;
        Context.ExpectedClosure        = Found.BlockParam;
        const SemaTypeId BlockType     = InferExpr( Context, Expr.BlockArg );
        Context.ExpectedClosure        = OuterExpected;

        // Found.BlockParam only ever fills a param slot that has nothing of
        // its own — a literal `do |i| ... end` under it always resolves. A
        // point-free `&transform` can still come out with an open slot when
        // `transform` itself was never annotated; that must stop here rather
        // than propagate as `Array<?>` with no diagnostic anywhere.
        if ( Found.Decl != nullptr and not IsBlockFullyInferred( Context, BlockType ) )
        {
            Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Expr.BlockArg ) ),
                            "cannot infer block parameter types for '" +
                                std::string{ Context.Ctx.Types.Text( Found.Decl->Name ) } +
                                "' — please add explicit type annotations" );
        }

        UnifyBlock( Context, Found, BlockType );
    }

    // Read after inference, not before: `Callee` is the result as it stood
    // when the member resolved, with its generic holes still open.
    return Found.Result;
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
