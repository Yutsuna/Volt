#include "ExprInferencer.hpp"

#include "ClosureInferencer.hpp"
#include "LiteralInferencer.hpp"
#include "MemberResolver.hpp"
#include "TypeCompat.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

/**
 * Private
 */

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

// Has this expression no type of its own yet, so that a surrounding context
// may decide it? True for a literal still awaiting narrowing, and for an
// identifier naming a local that was seeded by such a literal (`h = 5381`
// becoming UInt64 once `hash` demands one). False for everything else —
// notably a local with a written annotation, which is *checked* against its
// context, never rewritten by it.
[[nodiscard]] bool IsMalleable ( const Volt::Sema::TypeCheckerPass::TypeCheckerContext &Context, Volt::Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    if ( Context.UnconstrainedLiterals.contains( Id.Value ) )
    {
        return true;
    }
    const auto *Name = std::get_if<Volt::Frontend::Identifier>( &Context.Ctx.Ast.Expr( Id ) );
    return Name != nullptr and Context.UnconstrainedVarInitializers.contains( Name->Name );
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
        Context.Ctx.Ast.Add( Volt::Frontend::ExprNode{ Volt::Frontend::Identifier{ .Loc = {}, .Name = NameSym } } );
    const Volt::Frontend::ExprId NewMember = Context.Ctx.Ast.Add( Volt::Frontend::ExprNode{
        Volt::Frontend::Member{ .Loc = {}, .Object = ClassRef, .Name = Context.Ctx.Ast.Strings().Intern( "new" ) } } );
    Volt::Frontend::Call CallNode;
    CallNode.Callee = NewMember;
    if ( MessageArg.IsValid() )
    {
        CallNode.Args.PushBack( MessageArg );
    }
    return Context.Ctx.Ast.Add( Volt::Frontend::ExprNode{ std::move( CallNode ) } );
}

// `*p` yields what `p` points at. "Points at" is not a name the compiler
// knows: the pointer nominal is whichever stdlib type claims the PointerType
// node kind (`@[Literal( PointerType )]` on `Pointer<T>`), exactly the way
// NilLiteral identifies Nil in TypeCompat. The pointee is that instance's
// first generic argument.
[[nodiscard]] Volt::Sema::SemaTypeId
DerefType ( Volt::Sema::TypeCheckerPass::TypeCheckerContext &Context, Volt::Frontend::ExprId Id, Volt::Sema::SemaTypeId Operand )
{
    if ( not Context.Ctx.Values.Has( Operand ) )
    {
        return Volt::Sema::SemaTypeId{};
    }

    const Volt::Sema::SemaType &Value = Context.Ctx.Values.Get( Operand );
    const auto Pointer                = Context.Ctx.Types.LookupNodeKind( "PointerType" );
    if ( not Pointer or Value.Base != *Pointer )
    {
        Context.Report( Volt::Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ),
                        "cannot dereference a value of type " + Context.NameOfValue( Operand ) );
        return Volt::Sema::SemaTypeId{};
    }
    return Value.Args.IsEmpty() ? Volt::Sema::SemaTypeId{} : Value.Args[0];
}

} // namespace

/**
 * Public
 */

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

    if ( Context.bGenericBody )
    {
        Context.Ctx.Values.MarkDeferred( Id );
    }

    const SemaTypeId Type = ComputeExpr( Context, Id );
    Context.Ctx.Values.SetExprType( Id, Type );
    return Type;
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::ComputeExpr ( TypeCheckerContext &Context, Frontend::ExprId Id )
{
    const Frontend::ExprNode Node = Context.Ctx.Ast.Expr( Id );

    return std::visit(
        Meta::Overloaded{
            [&] ( const Frontend::SelfExpr & ) -> SemaTypeId
            {
                // No enclosing type body means no receiver. A module is a
                // namespace, not a type: its methods are registered as free
                // functions (Layout/TypeBinder.cpp), so `self` is invalid
                // there too and the call must be written bare.
                //
                // Reported here rather than left silent because DotCallLowering
                // turns an implicit `.method` into `self.method`: without this,
                // a receiver-less `.method` would type as "unknown" and vanish.
                if ( not Context.SelfValue.IsValid() )
                {
                    Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ),
                                    "'self' has no meaning outside a type body — a module is a namespace, not a type" );
                }

                // In a static method `self` *is* the type, exactly like writing
                // the type name: marking it naked is what lets the one member
                // check (CheckMemberSelf) reject `self.instance_member` there,
                // instead of a second static/instance predicate on the side.
                if ( Context.bStaticContext )
                {
                    Context.NakedTypeExprs.insert( Id.Value );
                }
                return Context.SelfValue;
            },
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
            { return MemberType( Context, Id, Context.SelfValue, Context.bStaticContext, Context.Ctx.Ast.Text( Expr.Name ) ); },
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
                // Was an inlined copy of MemberType: same lookup, same
                // recording, same unknown-member diagnostic, same self check.
                // Sharing the one function is what keeps a `Member` callee and
                // an operator receiver from drifting apart.
                // `MathUtils.square( 4 )` — a module is a namespace, so the
                // qualified spelling names the same free function the bare one
                // does (Layout/TypeBinder registers module methods flat). The
                // receiver is not a value and never gets a type; without this
                // the whole call typed as unknown, silently.
                const auto *Name = std::get_if<Frontend::Identifier>( &Context.Ctx.Ast.Expr( Expr.Object ) );
                if ( Name != nullptr and Context.Ctx.Types.IsModule( Context.Ctx.Ast.Text( Name->Name ) ) )
                {
                    // `Core::AppConfig` — a type declared inside the module.
                    // TypeBinder hoists nested types to the top level, so the
                    // qualified spelling names the same nominal the bare one
                    // does; naked, exactly like a bare type name.
                    if ( const auto Named = Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Expr.Name ) ) )
                    {
                        Context.NakedTypeExprs.insert( Id.Value );
                        return Context.MakeType( *Named, {} );
                    }
                    const Resolution Found = LookupFreeFunction( Context, Context.Ctx.Ast.Text( Expr.Name ) );
                    if ( Found.Decl != nullptr )
                    {
                        Context.CalleeResolution[Id.Value] = Found;
                        return Found.Result;
                    }
                    Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ),
                                    "module " + std::string{ Context.Ctx.Ast.Text( Name->Name ) } + " has no function '" +
                                        std::string{ Context.Ctx.Ast.Text( Expr.Name ) } + "'" );
                    return SemaTypeId{};
                }

                const SemaTypeId Object = InferExpr( Context, Expr.Object );

                return MemberType( Context, Id, Object, Context.NakedTypeExprs.contains( Expr.Object.Value ),
                                   Context.Ctx.Ast.Text( Expr.Name ) );
            },
            // No Frontend::Index branch: IndexLowering (order 25) rewrites
            // every subscript into a `[]` / `[]=` call before TypeChecker
            // runs. That is what gives an index a type at all — this branch
            // used to call MemberType directly and hand back an invalid
            // SemaTypeId on a generic receiver, silently.
            [&] ( const Frontend::Binary &Expr ) -> SemaTypeId
            {
                const SemaTypeId Lhs = InferExpr( Context, Expr.Lhs );
                const SemaTypeId Rhs = InferExpr( Context, Expr.Rhs );

                // An operand only adopts the other's type when it has none of
                // its own to lose (IsMalleable). The receiver is settled first,
                // because it is what the operator resolves on.
                if ( Rhs.IsValid() and not IsMalleable( Context, Expr.Rhs ) and IsMalleable( Context, Expr.Lhs ) )
                {
                    Context.ConstrainExprType( Expr.Lhs, Rhs );
                }

                // The resolution MemberType records is the whole of B.4: on a
                // primitive/pointer layout it is empty and the backend emits an
                // instruction chosen from `Primitive{ Spelling, Bits }`; on any
                // other layout it names the method to call. No lowering pass,
                // no node created — see rules/core-ast.md.
                const SemaTypeId Result =
                    MemberType( Context, Id, InferExpr( Context, Expr.Lhs ), Context.NakedTypeExprs.contains( Expr.Lhs.Value ),
                                Frontend::TokenSpelling( Expr.Op ) );

                // What the right operand is *expected* to be is written on the
                // operator's own declaration, so read it there rather than off
                // the receiver: `Arithmetic#+( other : self )` makes `x + 1`
                // adopt x's type, while `Pointer<T>#+( offset : UInt64 )` makes
                // `ptr + 1_u64` an offset. Taking the receiver's type — which
                // is what this did — is right only for the homogeneous
                // operators, and silently retyped every pointer offset to a
                // pointer, which a backend then cannot materialise as an
                // integer constant at all.
                SemaTypeId Expected;
                if ( const auto Entry = Context.CalleeResolution.find( Id.Value );
                     Entry != Context.CalleeResolution.end() and Entry->second.Params.Size() > 0 )
                {
                    Expected = Entry->second.Params[0];
                }
                if ( not Expected.IsValid() )
                {
                    Expected = Lhs;
                }
                if ( Expected.IsValid() and IsMalleable( Context, Expr.Rhs ) )
                {
                    Context.ConstrainExprType( Expr.Rhs, Expected );
                }
                return Result;
            },
            [&] ( const Frontend::Unary &Expr ) -> SemaTypeId
            {
                return MemberType( Context, Id, InferExpr( Context, Expr.Operand ),
                                   Context.NakedTypeExprs.contains( Expr.Operand.Value ), Frontend::TokenSpelling( Expr.Op ) );
            },
            [&] ( const Frontend::Deref &Expr ) -> SemaTypeId
            { return DerefType( Context, Id, InferExpr( Context, Expr.Operand ) ); },
            [&] ( const Frontend::SizeOf &Expr ) -> SemaTypeId
            {
                // A byte count takes its width from its use site — `count *
                // sizeof T` against a UInt64 count — exactly like an integer
                // literal does, so it joins UnconstrainedLiterals and falls
                // back to whatever type claims IntLiteral.
                //
                // Its operand is a type, never a value, so there is nothing to
                // descend into — but the *measured* type still has to be
                // published, or a backend would have to resolve the written
                // name itself, which is semantic analysis in codegen. It goes
                // on this node's own site: the site map is where a type
                // attached to an Id that is not a value expression lives, the
                // same channel a `rescue` clause's filter uses.
                UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Context.SelfValue, .Bindings = Context.GenericBindings() };
                Context.Ctx.Values.SetSiteType( BindingSite{ Id }, ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types,
                                                                                    Context.Generics(), Sink, Expr.Type ) );
                const auto Base = Context.Ctx.Types.LookupNodeKind( "IntLiteral" );
                if ( not Base )
                {
                    return SemaTypeId{};
                }
                Context.UnconstrainedLiterals.insert( Id.Value );
                return Context.MakeType( *Base, {} );
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
                        // Definite assignment: variable is now initialized.
                        Context.UninitializedLocals.erase( Target->Name );
                    }
                    else
                    {
                        Context.WriteLocal( Expr.Target, Target->Name, Value );
                        if ( Context.IsUnconstrainedInit( Expr.Value, Value ) )
                        {
                            Context.UnconstrainedVarInitializers[Target->Name] = Expr.Value;
                        }
                        Context.UninitializedLocals.erase( Target->Name );
                    }
                }
                // AssignLowering (order 24) has already turned `x op= v` into
                // `x = x op v`, so compound assignment reaches this one check
                // with no case of its own.
                const SemaTypeId TargetType = InferExpr( Context, Expr.Target );
                Context.ConstrainExprType( Expr.Value, TargetType );
                CheckAssignable( Context, Expr.Value, TargetType, EAssignSite::Assign );
                return Value;
            },
            [&] ( const Frontend::Ternary &Expr ) -> SemaTypeId
            {
                static_cast<void>( InferExpr( Context, Expr.Cond ) );
                SemaTypeId Then = InferExpr( Context, Expr.Then );
                SemaTypeId Else = InferExpr( Context, Expr.Else );

                // An arm that is still open — a bare literal — takes the
                // other's type, under the same IsMalleable guard the operands
                // of a Binary use. A ternary produces one value, so
                // `@capacity == 0 ? 8 : @capacity * 2` is a UInt64 in both
                // arms or it is nothing coherent: leaving the literal at its
                // default width made the two disagree, and a backend cannot
                // merge them at all.
                if ( Then.IsValid() and not IsMalleable( Context, Expr.Then ) and IsMalleable( Context, Expr.Else ) )
                {
                    Context.ConstrainExprType( Expr.Else, Then );
                    Else = Then;
                }
                else if ( Else.IsValid() and not IsMalleable( Context, Expr.Else ) and IsMalleable( Context, Expr.Then ) )
                {
                    Context.ConstrainExprType( Expr.Then, Else );
                    Then = Else;
                }
                return Context.UnifyBranchTypes( Then, Else );
            },
            [&] ( const Frontend::RaiseExpr &Expr ) -> SemaTypeId
            {
                // Copy out, compute, write back — rules/ast-rewrite.md. Every
                // branch below calls `Add()`, the arena is a `std::vector`,
                // and a reference into a node taken beforehand (which is what
                // `Expr` itself is, since std::visit binds it into the arena)
                // is dangling the moment one reallocates. The Id is stable;
                // the reference never was.
                const Core::SourceRange Loc = Expr.Loc;
                Frontend::ExprId Exception  = Expr.Exception;

                if ( not Exception.IsValid() )
                {
                    // Bare `raise`: re-raise the innermost rescue's bound
                    // variable, or (outside any rescue) fall back to
                    // constructing a fresh exception.
                    if ( not Context.RescueVarStack.empty() and Context.RescueVarStack.back().IsValid() )
                    {
                        Exception = Context.Ctx.Ast.Add(
                            Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = Context.RescueVarStack.back() } } );
                    }
                    else
                    {
                        Exception = MakeExceptionConstructor( Context, Loc, Frontend::ExprId{} );
                    }
                }
                else if ( std::holds_alternative<Frontend::StringLiteral>( Context.Ctx.Ast.Expr( Exception ) ) or
                          std::holds_alternative<Frontend::Interp>( Context.Ctx.Ast.Expr( Exception ) ) )
                {
                    // `raise "msg"` desugars to `Exception.new("msg")`, moved
                    // here from the parser so the constructed callee resolves
                    // through TypeStore's @[ExceptionRoot] instead of a
                    // hardcoded name.
                    Exception = MakeExceptionConstructor( Context, Loc, Exception );
                }

                std::get<Frontend::RaiseExpr>( Context.Ctx.Ast.Expr( Id ) ).Exception = Exception;
                if ( Exception.IsValid() )
                {
                    static_cast<void>( InferExpr( Context, Exception ) );
                }
                return TypeCheckerContext::NoReturnType();
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
                        UnitSink Sink{
                            .Values = Context.Ctx.Values, .Self = Context.SelfValue, .Bindings = Context.GenericBindings() };
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
                    // Recorded whether or not the clause binds a name: a backend
                    // matching this clause's filter against the in-flight exception's
                    // dynamic type needs the *resolved* nominal regardless of whether
                    // the clause also captures it, and re-resolving Clause.ExceptionType
                    // itself would be semantic analysis in codegen. Keyed by the
                    // clause's own StmtId — ScopeResolver's BindingSite{Id} for a
                    // RescueClause, the same site a bound variable's slot uses below.
                    Context.Ctx.Values.SetSiteType( BindingSite{ ClauseId }, ExceptionType );
                    if ( Clause.VarName.IsValid() )
                    {
                        // WriteLocal resolves its Site through Scopes.BindingOf(Use), a
                        // *use* -> declaration index — there is no "use" expression for a
                        // rescue clause's own binding, only its declaration, so it cannot
                        // reach SetSiteType above. Name-based typing still goes through
                        // Locals/LocalTypes exactly as DeclStmtWalker's LocalDecl does.
                        Context.LocalTypes[BindingSite{ ClauseId }] = ExceptionType;
                        Context.LocalSites[Clause.VarName]          = BindingSite{ ClauseId };
                        Context.Locals[Clause.VarName]              = ExceptionType;
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
            // No Frontend::DotCall branch: DotCallLowering (order 23) rewrites
            // every one into `self.method( ... )` before TypeChecker (order 30)
            // runs, and RunPasses always runs the Lowering passes first. The
            // node reaching here would be a bug the AstInvariant pass reports.
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
            // `( Value : Type )` — explicit ascription, not a cast. Resolve
            // `Type` and constrain `Value` against it exactly like a
            // `LocalDecl`'s written type does (`DeclStmtWalker.cpp`): once
            // before inferring `Value` (so an unconstrained lambda/closure
            // body sees the expectation) and once after (a literal only
            // joins `UnconstrainedLiterals` while being inferred, so the
            // first call cannot narrow it).
            [&] ( const Frontend::TypedExpr &Expr ) -> SemaTypeId
            {
                UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Context.SelfValue, .Bindings = Context.GenericBindings() };
                const SemaTypeId Written =
                    ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Expr.Type );
                if ( Written.IsValid() )
                {
                    Context.ConstrainExprType( Expr.Value, Written );
                }
                const SemaTypeId ValueType = InferExpr( Context, Expr.Value );
                if ( Written.IsValid() )
                {
                    Context.ConstrainExprType( Expr.Value, Written );
                }
                return Written.IsValid() ? Written : ValueType;
            },
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
    for ( std::size_t ArgIdx = 0; ArgIdx < Expr.Args.Size(); ++ArgIdx )
    {
        const Frontend::ExprId Arg = Expr.Args[ArgIdx];

        // When the argument is a closure literal and the callee declares a
        // callable parameter at that position, feed the parameter's type as
        // ExpectedClosure so the closure's unannotated parameters get their
        // types — the same mechanism BlockArg already uses for trailing
        // blocks, extended here to positional arguments.
        const bool bArgIsClosure = Arg.IsValid() and ( std::holds_alternative<Frontend::Lambda>( Context.Ctx.Ast.Expr( Arg ) ) or
                                                       std::holds_alternative<Frontend::Block>( Context.Ctx.Ast.Expr( Arg ) ) );
        const SemaTypeId OuterExpected = Context.ExpectedClosure;

        if ( bArgIsClosure and ArgIdx < Found.Params.Size() and Found.Params[ArgIdx].IsValid() )
        {
            Context.ExpectedClosure = Found.Params[ArgIdx];
        }

        static_cast<void>( InferExpr( Context, Arg ) );

        if ( bArgIsClosure )
        {
            Context.ExpectedClosure = OuterExpected;
        }
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
        UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Context.SelfValue, .Bindings = Context.GenericBindings() };
        Args.PushBack( ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Arg ) );
    }

    CheckArity( Context, Expr.Loc, Nominal, Args.Size() );
    return Context.MakeType( Nominal, std::move( Args ) );
}
