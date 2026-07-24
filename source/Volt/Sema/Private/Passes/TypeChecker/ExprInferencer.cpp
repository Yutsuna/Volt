#include "ExprInferencer.hpp"

#include "ClosureInferencer.hpp"
#include "LiteralInferencer.hpp"
#include "MemberResolver.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

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

// Only `Args[0]` (the `Lambda`/`Block` nominal's Result, see ClosureType) is
// checked, and only for a `Lambda` node — never a `Block`. The two differ in
// exactly the way that matters here:
//
// - A `do |x| ... end` (`Block`) is a statement sequence with no trailing
//   expression more often than not — `each do |item| ... end` never types
//   one — and its parameter slots are legitimately open while checking a
//   generic definition's own body (`T` inside `Enumerable<T>` itself is an
//   unresolved placeholder, not a failure — see UnitSink::Param). Neither
//   signal is trustworthy there, so `Block` is skipped outright.
// - Every `&expr` capture is rewritten to a `Lambda` by FunctionalLowering
//   (`(fn_tmp) => expr(fn_tmp)`) before this pass ever runs, and a `Lambda`
//   body is always a single expression meant to produce a value. Its own
//   parameter always resolves — it is filled from the call's `Expected`
//   slot mechanically, regardless of what `expr` turns out to be — so a
//   parameter check would fire even in a fully generic definition body.
//   What actually goes missing when `expr` (e.g. a local closure declared
//   without annotations) was never resolved is the *Result*: exactly what
//   feeds a method generic like `map<U>`'s `U`, and exactly what silently
//   produced `Array<?>` before this check existed.
[[nodiscard]] bool IsLambdaExpr ( const Volt::Frontend::AstContext &Ast, Volt::Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    return std::holds_alternative<Volt::Frontend::Lambda>( Ast.Expr( Id ) );
}

[[nodiscard]] bool IsBlockResultInferred ( const Volt::Sema::TypeCheckerPass::TypeCheckerContext &Context,
                                           Volt::Frontend::ExprId BlockArg,
                                           Volt::Sema::SemaTypeId BlockType )
{
    if ( not std::holds_alternative<Volt::Frontend::Lambda>( Context.Ctx.Ast.Expr( BlockArg ) ) )
    {
        return true;
    }
    if ( not BlockType.IsValid() or not Context.Ctx.Values.Has( BlockType ) )
    {
        return false;
    }
    const auto &Slots = Context.Ctx.Values.Get( BlockType ).Args;
    return not Slots.IsEmpty() and Slots[0].IsValid();
}

// The nominal `Type` resolves to, or an invalid handle when `Type` never
// resolved (e.g. an unbound receiver) — Values.Get is only safe once Has()
// is confirmed.
[[nodiscard]] Volt::Sema::NominalId ScrutineeNominal ( const Volt::Sema::TypeCheckerPass::TypeCheckerContext &Context,
                                                       Volt::Sema::SemaTypeId Type )
{
    if ( not Type.IsValid() or not Context.Ctx.Values.Has( Type ) )
    {
        return {};
    }
    return Context.Ctx.Values.Get( Type ).Base;
}

// The enum case name a desugared `when` pattern selects, if any.
// `CaseLowering` (Order 22) turns `.Name` into `target.Name()` — a `Call`
// over a `Member` — and any other pattern into `pattern === target` once
// there is a target, so a written `TaskStatus::InProgress` (an explicit
// receiver, never a `.Name` DotCall) surfaces as the `Call`/`Member` nested
// inside that `Binary`. Recognizing both shapes here, recursively, is what
// lets exhaustiveness see through either spelling without CaseLowering
// having to know anything about enums itself.
[[nodiscard]] std::optional<std::string_view> EnumCaseNameOf ( const Volt::Frontend::AstContext &Ast,
                                                               Volt::Frontend::ExprId Pattern )
{
    if ( not Pattern.IsValid() )
    {
        return std::nullopt;
    }

    if ( const auto *Bin = std::get_if<Volt::Frontend::Binary>( &Ast.Expr( Pattern ) );
         Bin != nullptr and Bin->Op == Volt::Frontend::TokenKind::TripleEq )
    {
        if ( const auto Name = EnumCaseNameOf( Ast, Bin->Lhs ) )
        {
            return Name;
        }
        return EnumCaseNameOf( Ast, Bin->Rhs );
    }

    // `TrafficLight::Red` (an explicit receiver) parses as a bare `Member` —
    // `::` is just `.` to the parser (ParsePostfix) — never a `Call`; only
    // the `.Name` sugar goes through the DotCall→Call(Member) rewrite above.
    if ( const auto *BareMember = std::get_if<Volt::Frontend::Member>( &Ast.Expr( Pattern ) ); BareMember != nullptr )
    {
        return Ast.Text( BareMember->Name );
    }

    const auto *CallNode = std::get_if<Volt::Frontend::Call>( &Ast.Expr( Pattern ) );
    if ( CallNode == nullptr )
    {
        return std::nullopt;
    }
    const auto *MemberNode = std::get_if<Volt::Frontend::Member>( &Ast.Expr( CallNode->Callee ) );
    if ( MemberNode == nullptr )
    {
        return std::nullopt;
    }
    return Ast.Text( MemberNode->Name );
}

// Types a `case` / `case target` expression and, when the scrutinee is an
// enum, diagnoses a `when` that neither covers every case nor carries an
// `else`. The expression's own value is the first clause (or the `else`)
// whose trailing statement produced one — same policy `Ternary` already
// uses for `Then`/`Else`.
[[nodiscard]] Volt::Sema::SemaTypeId CaseType ( Volt::Sema::TypeCheckerPass::TypeCheckerContext &Context,
                                                Volt::Frontend::ExprId Id,
                                                const Volt::Frontend::CaseExpr &Expr )
{
    using namespace Volt::Sema::TypeCheckerPass;

    const bool bHasTarget                      = Expr.Target.IsValid();
    const Volt::Sema::SemaTypeId ScrutineeType = bHasTarget ? InferExpr( Context, Expr.Target ) : Context.SelfValue;
    const Volt::Sema::NominalId Nominal        = ScrutineeNominal( Context, ScrutineeType );
    const bool bIsEnum                         = HasEnumCases( Context.Ctx.Types, Nominal );

    std::unordered_set<std::string_view> Covered;
    Volt::Sema::SemaTypeId Result;

    for ( const Volt::Frontend::StmtId ClauseId : Expr.Clauses )
    {
        if ( not ClauseId.IsValid() or
             Volt::Frontend::KindOf( Context.Ctx.Ast.Stmt( ClauseId ) ) != Volt::Frontend::StmtKind::WhenClause )
        {
            continue;
        }
        const auto &Clause = std::get<Volt::Frontend::WhenClause>( Context.Ctx.Ast.Stmt( ClauseId ) );

        for ( const Volt::Frontend::ExprId Pattern : Clause.Patterns )
        {
            static_cast<void>( InferExpr( Context, Pattern ) );
            if ( not bIsEnum )
            {
                continue;
            }
            if ( const auto Name = EnumCaseNameOf( Context.Ctx.Ast, Pattern ) )
            {
                if ( const Volt::Sema::Member *CaseMember = Context.Ctx.Types.OwnMember( Nominal, *Name );
                     CaseMember != nullptr and CaseMember->Kind == Volt::Sema::EMemberKind::EnumCase )
                {
                    Covered.insert( *Name );
                }
            }
        }

        const Volt::Sema::SemaTypeId ClauseType = TrailingType( Context, Clause.Body );
        Result                                  = Context.UnifyBranchTypes( Result, ClauseType );
    }

    const Volt::Sema::SemaTypeId ElseType = TrailingType( Context, Expr.ElseBody );
    Result                                = Context.UnifyBranchTypes( Result, ElseType );

    if ( bIsEnum and Expr.ElseBody.IsEmpty() )
    {
        std::string Missing;
        for ( const Volt::Sema::Member &CaseMember : Context.Ctx.Types.Type( Nominal ).Members )
        {
            if ( CaseMember.Kind != Volt::Sema::EMemberKind::EnumCase )
            {
                continue;
            }
            const std::string_view Name = Context.Ctx.Types.Text( CaseMember.Name );
            if ( Covered.contains( Name ) )
            {
                continue;
            }
            Missing += Missing.empty() ? "'" : ", '";
            Missing += std::string{ Name } + "'";
        }
        if ( not Missing.empty() )
        {
            Context.Report( Volt::Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ),
                            "non-exhaustive case: missing variant(s) " + Missing + " for type " + Context.NameOf( Nominal ) );
        }
    }

    return Result;
}

// Builds `<ExceptionRoot>.new(MessageArg)` (MessageArg may be an invalid
// ExprId for a bare `.new()`), resolving the root type's name dynamically
// through TypeStore rather than a hardcoded spelling. Shared by the two
// places a `raise` needs to materialise an actual exception value: the
// `raise "msg"` string-literal desugar and the bare-`raise`-with-no-active-
// rescue fallback.
[[nodiscard]] Volt::Frontend::ExprId MakeExceptionConstructor ( Volt::Sema::TypeCheckerPass::TypeCheckerContext &Context,
                                                                Volt::Core::SourceRange Loc,
                                                                Volt::Frontend::ExprId MessageArg )
{
    const auto Root = Context.Ctx.Types.GetExceptionRoot();
    if ( not Root )
    {
        Context.Report( Loc, "no type is annotated @[ExceptionRoot]; the stdlib must declare one" );
        return Volt::Frontend::ExprId{};
    }
    const std::string_view RootName      = Context.Ctx.Types.Text( Context.Ctx.Types.Type( *Root ).Name );
    const Volt::Frontend::Symbol NameSym = Context.Ctx.Ast.Strings().Intern( RootName );
    const Volt::Frontend::ExprId ClassRef =
        Context.Ctx.Ast.Add( Volt::Frontend::ExprNode{ Volt::Frontend::Identifier{ {}, NameSym } } );
    const Volt::Frontend::ExprId NewMember = Context.Ctx.Ast.Add(
        Volt::Frontend::ExprNode{ Volt::Frontend::Member{ {}, ClassRef, Context.Ctx.Ast.Strings().Intern( "new" ) } } );
    Volt::Frontend::Call CallNode;
    CallNode.Callee = NewMember;
    if ( MessageArg.IsValid() )
    {
        CallNode.Args.PushBack( MessageArg );
    }
    return Context.Ctx.Ast.Add( Volt::Frontend::ExprNode{ std::move( CallNode ) } );
}

} // namespace

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::ComputeExpr ( TypeCheckerContext &Context, Frontend::ExprId Id )
{
    const Frontend::ExprNode &Node = Context.Ctx.Ast.Expr( Id );

    return std::visit(
        Meta::Overloaded{
            [&] ( const Frontend::SelfExpr & ) -> SemaTypeId { return Context.SelfValue; },
            [&] ( const Frontend::SuperExpr & ) -> SemaTypeId
            {
                if ( Context.SelfValue.IsValid() )
                {
                    const SemaType &SelfConc = Context.Ctx.Values.Get( Context.SelfValue );
                    if ( SelfConc.Base.IsValid() )
                    {
                        const NominalType &NomType = Context.Ctx.Types.Type( SelfConc.Base );
                        if ( NomType.Super.IsValid() )
                        {
                            const SemaTypeId SuperInstance = Instantiate( Context.Ctx.Types, NomType.Super, SelfConc.Args,
                                                                          Context.SelfValue, Context.Ctx.Values );
                            const Resolution Found         = LookupOn( Context, SuperInstance, "initialize" );
                            if ( Found.Decl != nullptr )
                            {
                                Context.CalleeResolution[Id.Value] = Found;
                                return Found.Result;
                            }
                        }
                    }
                }
                return SemaTypeId{};
            },
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

                if ( Context.Ctx.Ast.Text( Expr.Name ) == "super" and Context.SelfValue.IsValid() )
                {
                    const SemaType &SelfConc = Context.Ctx.Values.Get( Context.SelfValue );
                    if ( SelfConc.Base.IsValid() )
                    {
                        const NominalType &NomType = Context.Ctx.Types.Type( SelfConc.Base );
                        if ( NomType.Super.IsValid() )
                        {
                            const SemaTypeId SuperInstance = Instantiate( Context.Ctx.Types, NomType.Super, SelfConc.Args,
                                                                          Context.SelfValue, Context.Ctx.Values );
                            const Resolution Found         = LookupOn( Context, SuperInstance, "initialize" );
                            if ( Found.Decl != nullptr )
                            {
                                Context.CalleeResolution[Id.Value] = Found;
                                return Found.Result;
                            }
                        }
                    }
                }

                // Neither a local nor a type: inside a method body, a bare
                // name is a member of `self`. Resolving it here is what makes
                // `each do | item | ... end` behave like `self.each do ... end`
                // — same CalleeResolution, so the block gets its parameter
                // types and the method generics still infer.
                if ( Context.SelfValue.IsValid() )
                {
                    const Resolution Found = LookupOn( Context, Context.SelfValue, Context.Ctx.Ast.Text( Expr.Name ) );
                    if ( Found.Decl != nullptr )
                    {
                        Context.CalleeResolution[Id.Value] = Found;
                        return Found.Result;
                    }
                }

                // Neither a member of `self` (or no `self` at all, as at
                // module scope): a top-level `def`. CallType diagnoses the
                // case where even this fails and the identifier is used as a
                // call's callee — every other silent fallthrough here stays
                // silent, since a bare name may legitimately mean something
                // a later lowering pass still has to fill in.
                const Resolution FreeFn = LookupFreeFunction( Context, Context.Ctx.Ast.Text( Expr.Name ) );
                if ( FreeFn.Decl != nullptr )
                {
                    Context.CalleeResolution[Id.Value] = FreeFn;
                    return FreeFn.Result;
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
                return Context.UnifyBranchTypes( Then, Else );
            },
            [&] ( const Frontend::RaiseExpr &Expr ) -> SemaTypeId
            {
                Frontend::RaiseExpr &Mutable = std::get<Frontend::RaiseExpr>( Context.Ctx.Ast.Expr( Id ) );
                if ( not Mutable.Exception.IsValid() )
                {
                    // Bare `raise`: re-raise the innermost rescue's bound
                    // variable, or (outside any rescue) fall back to
                    // constructing a fresh exception.
                    if ( not Context.RescueVarStack.empty() and Context.RescueVarStack.back().IsValid() )
                    {
                        Mutable.Exception = Context.Ctx.Ast.Add(
                            Frontend::ExprNode{ Frontend::Identifier{ {}, Context.RescueVarStack.back() } } );
                    }
                    else
                    {
                        Mutable.Exception = MakeExceptionConstructor( Context, Expr.Loc, Frontend::ExprId{} );
                    }
                }
                else if ( std::holds_alternative<Frontend::StringLiteral>( Context.Ctx.Ast.Expr( Mutable.Exception ) ) or
                          std::holds_alternative<Frontend::Interp>( Context.Ctx.Ast.Expr( Mutable.Exception ) ) )
                {
                    // `raise "msg"` desugars to `Exception.new("msg")`, moved
                    // here from the parser so the constructed callee resolves
                    // through TypeStore's @[ExceptionRoot] instead of a
                    // hardcoded name.
                    Mutable.Exception = MakeExceptionConstructor( Context, Expr.Loc, Mutable.Exception );
                }
                if ( Mutable.Exception.IsValid() )
                {
                    static_cast<void>( InferExpr( Context, Mutable.Exception ) );
                }
                return Context.NoReturnType();
            },
            [&] ( const Frontend::BeginExpr &Expr ) -> SemaTypeId
            {
                SemaTypeId Result = TrailingType( Context, Expr.Body );
                for ( const Frontend::StmtId ClauseId : Expr.RescueClauses )
                {
                    if ( not ClauseId.IsValid() )
                    {
                        continue;
                    }
                    const auto &Clause = std::get<Frontend::RescueClause>( Context.Ctx.Ast.Stmt( ClauseId ) );
                    SemaTypeId ExceptionType{};
                    if ( Clause.ExceptionType.IsValid() )
                    {
                        UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Context.SelfValue };
                        ExceptionType =
                            ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Clause.ExceptionType );
                        const NominalId Nominal =
                            Context.Ctx.Values.Has( ExceptionType ) ? Context.Ctx.Values.Get( ExceptionType ).Base : NominalId{};
                        if ( const auto ExcOpt = Context.Ctx.Types.GetExceptionRoot(); ExcOpt.has_value() and Nominal.IsValid() )
                        {
                            if ( not IsSubclassOf( Context.Ctx.Types, Nominal, *ExcOpt ) )
                            {
                                Context.Report( Frontend::LocOf( Context.Ctx.Ast.Stmt( ClauseId ) ),
                                                "rescue filter type must be a subclass of Exception" );
                            }
                        }
                    }
                    else if ( const auto ExcOpt = Context.Ctx.Types.GetExceptionRoot(); ExcOpt.has_value() )
                    {
                        ExceptionType = Context.MakeType( *ExcOpt, {} );
                    }
                    if ( Clause.VarName.IsValid() )
                    {
                        Context.WriteLocal( Frontend::ExprId{}, Clause.VarName, ExceptionType );
                    }
                    Context.RescueVarStack.push_back( Clause.VarName );
                    const SemaTypeId ClauseType = TrailingType( Context, Clause.Body );
                    Context.RescueVarStack.pop_back();
                    Result = Context.UnifyBranchTypes( Result, ClauseType );
                }
                if ( not Expr.EnsureBody.IsEmpty() )
                {
                    static_cast<void>( TrailingType( Context, Expr.EnsureBody ) );
                }
                return Result;
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
            [&] ( const Frontend::CaseExpr &Expr ) -> SemaTypeId { return CaseType( Context, Id, Expr ); },
            [&] ( const auto &Expr ) -> SemaTypeId { return LiteralType( Context, Id, Expr ); },
        },
        Node );
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::CallType ( TypeCheckerContext &Context, const Frontend::Call &Expr )
{
    if ( IsLambdaExpr( Context.Ctx.Ast, Expr.Callee ) and not Expr.Args.IsEmpty() and not Context.ExpectedClosure.IsValid() )
    {
        Core::SmallVec<SemaTypeId, 2> ArgTypes;
        for ( const Frontend::ExprId Arg : Expr.Args )
        {
            ArgTypes.PushBack( InferExpr( Context, Arg ) );
        }
        Context.ExpectedClosure = ClosureType( Context, "Lambda", SemaTypeId{}, ArgTypes );
    }

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
        // A bare identifier that is neither a local, a type, a member of
        // `self`, nor a declared free function: unlike a `receiver.name`
        // (Member) or `self.name` (DotCall) call, nothing upstream of here
        // ever reports this — those two go through LookupOn/MemberType,
        // which already diagnose an unknown member. This is that same
        // diagnostic's counterpart for a plain `foo( ... )` call.
        if ( not Callee.IsValid() )
        {
            if ( const auto *Naked = std::get_if<Frontend::Identifier>( &Context.Ctx.Ast.Expr( Expr.Callee ) ) )
            {
                Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Expr.Callee ) ),
                                "unknown function '" + std::string{ Context.Ctx.Ast.Text( Naked->Name ) } + "'" );
            }
        }

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
        if ( Found.Decl != nullptr and not IsBlockResultInferred( Context, Expr.BlockArg, BlockType ) )
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
