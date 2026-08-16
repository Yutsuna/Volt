#include "ExprInferencer.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"

#include "ClosureInferencer.hpp"
#include "LiteralInferencer.hpp"
#include "MemberResolver.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/MiddleEnd/ConstEval/TraitEngine.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeCompat.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"

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

// A bare naked-type name (`Optional`, no `<T>` written) used as a receiver
// still needs one binding slot per the type's own generic parameter, or
// nothing downstream (`LookupOn`/`Reinstantiate`/`UnifyArgs`) has anywhere
// to record what `T` turns out to be — `Optional::Some( "Yutsuna" )` would
// reinstantiate to an unbound `Optional<T>` and fail `IsAssignable` against
// a declared `Optional<String>`. Same placeholder-slot shape `Reinstantiate`
// already uses for `Found.Decl->OwnGenerics` (`MemberResolver.cpp`), just
// keyed on the *type's* own `Params` rather than a method's.
[[nodiscard]] Volt::Core::SmallVec<Volt::MiddleEnd::TypeSystem::SemaTypeId, 2>
PlaceholderTypeArgs ( const Volt::MiddleEnd::TypeSystem::TypeStore &Store, Volt::MiddleEnd::TypeSystem::NominalId Named )
{
    Volt::Core::SmallVec<Volt::MiddleEnd::TypeSystem::SemaTypeId, 2> Args;
    for ( std::size_t Index = 0; Index < Store.Type( Named ).Params.Size(); ++Index )
    {
        Args.PushBack( Volt::MiddleEnd::TypeSystem::SemaTypeId{} );
    }
    return Args;
}

[[nodiscard]] bool IsBlockResultInferred ( const Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                                           Volt::Frontend::ExprId BlockArg,
                                           Volt::MiddleEnd::TypeSystem::SemaTypeId BlockType )
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
[[nodiscard]] bool IsMalleable ( const Volt::MiddleEnd::Analysis::TypeCheckerContext &Context, Volt::Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    if ( Context.UnconstrainedLiterals.contains( Id.Value ) )
    {
        return true;
    }
    const auto &Node = Context.Ctx.Ast.Expr( Id );
    const auto *Name = std::get_if<Volt::Frontend::Identifier>( &Node );
    if ( Name != nullptr )
    {
        return Context.UnconstrainedVarInitializers.contains( Name->Name );
    }
    // `-1` is a `Unary` over the still-unconstrained literal `1` (the parser
    // never folds a sign into a literal token): a comparison operand written
    // this way — `~zero == -1` — is exactly as malleable as the literal it
    // wraps, or the cross-operand propagation below never reaches it and the
    // literal keeps its default width.
    if ( const auto *UnaryNode = std::get_if<Volt::Frontend::Unary>( &Node ); UnaryNode != nullptr )
    {
        return IsMalleable( Context, UnaryNode->Operand );
    }
    return false;
}

// The nominal `Type` resolves to, or an invalid handle when `Type` never
// resolved (e.g. an unbound receiver) — Values.Get is only safe once Has()
// is confirmed.
[[nodiscard]] Volt::MiddleEnd::TypeSystem::NominalId
ScrutineeNominal ( const Volt::MiddleEnd::Analysis::TypeCheckerContext &Context, Volt::MiddleEnd::TypeSystem::SemaTypeId Type )
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
[[nodiscard]] Volt::MiddleEnd::TypeSystem::SemaTypeId CaseType ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                                                                 Volt::Frontend::ExprId Id,
                                                                 const Volt::Frontend::CaseExpr &Expr )
{
    using namespace Volt::MiddleEnd::Analysis;

    const Volt::Frontend::ExprId ScrutineeId = Expr.Scrutinee.IsValid() ? Expr.Scrutinee : Expr.Target;
    const bool bHasTarget                    = ScrutineeId.IsValid();
    const Volt::MiddleEnd::TypeSystem::SemaTypeId ScrutineeType =
        bHasTarget ? InferExpr( Context, ScrutineeId ) : Context.SelfValue;
    const Volt::MiddleEnd::TypeSystem::NominalId Nominal = ScrutineeNominal( Context, ScrutineeType );
    const bool bIsEnum                                   = HasEnumCases( Context.Ctx.Types, Nominal );

    std::unordered_set<std::string_view> Covered;
    Volt::MiddleEnd::TypeSystem::SemaTypeId Result;

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
                if ( const Volt::MiddleEnd::TypeSystem::Member *CaseMember = Context.Ctx.Types.OwnMember( Nominal, *Name );
                     CaseMember != nullptr and CaseMember->Kind == Volt::MiddleEnd::TypeSystem::EMemberKind::EnumCase )
                {
                    Covered.insert( *Name );
                }
            }
        }

        const Volt::MiddleEnd::TypeSystem::SemaTypeId ClauseType = TrailingType( Context, Clause.Body );
        Result                                                   = Context.UnifyBranchTypes( Result, ClauseType );
    }

    const Volt::MiddleEnd::TypeSystem::SemaTypeId ElseType = TrailingType( Context, Expr.ElseBody );
    Result                                                 = Context.UnifyBranchTypes( Result, ElseType );

    if ( bIsEnum and Expr.ElseBody.IsEmpty() )
    {
        std::string Missing;
        for ( const Volt::MiddleEnd::TypeSystem::Member &CaseMember : Context.Ctx.Types.Type( Nominal ).Members )
        {
            if ( CaseMember.Kind != Volt::MiddleEnd::TypeSystem::EMemberKind::EnumCase )
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

// Builds `<Root>.new(MessageArg)` (MessageArg may be an invalid
// ExprId for a bare `.new()`), resolving the root type's name dynamically
// through TypeStore rather than a hardcoded spelling. Shared by the two
// places a `raise` needs to materialise an actual exception value: the
// `raise "msg"` string-literal desugar and the bare-`raise`-with-no-active-
// rescue fallback.
[[nodiscard]] Volt::Frontend::ExprId MakeExceptionConstructor ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                                                                Volt::Core::SourceRange Loc,
                                                                Volt::Frontend::ExprId MessageArg )
{
    const auto Root = Context.Ctx.Types.LookupNodeKind( "RaiseExpr" );
    if ( not Root )
    {
        Context.Report( Loc, "no type claims @[Literal( RaiseExpr )]; the stdlib must declare one" );
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
[[nodiscard]] Volt::MiddleEnd::TypeSystem::SemaTypeId DerefType ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                                                                  Volt::Frontend::ExprId Id,
                                                                  Volt::MiddleEnd::TypeSystem::SemaTypeId Operand )
{
    if ( not Context.Ctx.Values.Has( Operand ) )
    {
        return Volt::MiddleEnd::TypeSystem::SemaTypeId{};
    }

    const Volt::MiddleEnd::TypeSystem::SemaType &Value = Context.Ctx.Values.Get( Operand );
    const auto Pointer                                 = Context.Ctx.Types.LookupNodeKind( "PointerType" );
    if ( not Pointer or Value.Base != *Pointer )
    {
        Context.Report( Volt::Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ),
                        "cannot dereference a value of type " + Context.NameOfValue( Operand ) );
        return Volt::MiddleEnd::TypeSystem::SemaTypeId{};
    }
    return Value.Args.IsEmpty() ? Volt::MiddleEnd::TypeSystem::SemaTypeId{} : Value.Args[0];
}

// --- Receiver traits -------------------------------------------------------
// `user.is_a? Admin`, `row.has_field? :id` — see ConstEval/TraitEngine.hpp.
//
// Folded here, at the *callee* seam, rather than after ordinary resolution,
// because there is nothing to resolve: no type declares `is_a?`, so letting
// the Member callee infer first would report "type Foo has no member 'is_a?'"
// before this ever ran. Only the receiver is inferred; the trait's own Member
// node is never typed and never reaches CalleeResolution, which is what makes
// "no call is emitted" structural rather than a promise — there is no
// Resolution for a backend to emit a call from.

// The BoolLiteral answer's own type — a node-kind claim, the compiler's own
// vocabulary, never a Volt type name (rules/zero-hardcode.md). The same line
// the TypeTrait arm uses, for the same reason: there is exactly one truth type
// and the stdlib is what says which.
[[nodiscard]] Volt::MiddleEnd::TypeSystem::SemaTypeId TruthType ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context )
{
    const auto Base = Context.Ctx.Types.LookupNodeKind( "BoolLiteral" );
    return Base ? Context.MakeType( *Base, {} ) : Volt::MiddleEnd::TypeSystem::SemaTypeId{};
}

// Replaces `Target`'s slot with the `BoolLiteral` answer, unless this is a
// re-instantiation walk (`Redirects`), in which case the shared generic body
// is left untouched and the substitution recorded instead — every
// instantiation reaches this same node with a different receiver and needs its
// own answer. Identical in shape and reason to ClosureLifting's own
// RewriteSlot.
//
// `Truth` is published on whichever node the backend will actually read: the
// target in mutate mode, the *new* node in redirect mode. Skipping the latter
// is how an instantiated body reaches codegen holding a literal nobody typed.
void FoldInto ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                Volt::Frontend::ExprId Target,
                Volt::Core::SourceRange Loc,
                bool Answer,
                Volt::MiddleEnd::TypeSystem::SemaTypeId Truth )
{
    Volt::Frontend::ExprNode Content{ Volt::Frontend::BoolLiteral{ .Loc = Loc, .Value = Answer } };

    if ( Context.Redirects != nullptr )
    {
        const Volt::Frontend::ExprId Folded  = Context.Ctx.Ast.Add( std::move( Content ) );
        ( *Context.Redirects )[Target.Value] = Folded;
        Context.Ctx.Values.SetExprType( Folded, Truth );
        return;
    }
    Context.Ctx.Ast.Expr( Target ) = std::move( Content );
    Context.Ctx.Values.SetExprType( Target, Truth );
}

// The nominal `Id` names when it is written as a bare type (`obj.is_a? Admin`)
// — resolved through the ordinary naked-type path rather than by reading the
// spelling, so a qualified name, an alias and `Vector<Int32>` all work and
// none of them is a case here. A generic's arguments are dropped on purpose:
// see TraitSite's own comment.
[[nodiscard]] Volt::MiddleEnd::TypeSystem::NominalId OperandNominal ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                                                                      Volt::Frontend::ExprId Id )
{
    using namespace Volt::MiddleEnd::Analysis;
    const Volt::MiddleEnd::TypeSystem::SemaTypeId Named = InferExpr( Context, Id );
    if ( not Context.NakedTypeExprs.contains( Id.Value ) or not Context.Ctx.Values.Has( Named ) )
    {
        return Volt::MiddleEnd::TypeSystem::NominalId{};
    }
    return Context.Ctx.Values.Get( Named ).Base;
}

// Answers `Id` when it is a receiver trait, and reports whether it was one.
// `Out` is the folded node's type; the node itself has been replaced.
[[nodiscard]] bool FoldReceiverTrait ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                                       Volt::Frontend::ExprId Id,
                                       const Volt::Frontend::Call &Expr,
                                       Volt::MiddleEnd::TypeSystem::SemaTypeId &Out )
{
    namespace ConstEval = Volt::MiddleEnd::ConstEval;
    using Volt::MiddleEnd::TypeSystem::NominalId;

    const auto *Callee = std::get_if<Volt::Frontend::Member>( &Context.Ctx.Ast.Expr( Expr.Callee ) );
    if ( Callee == nullptr )
    {
        return false;
    }
    const auto Trait = ConstEval::LookupTrait( Context.Ctx.Ast.Text( Callee->Name ) );
    if ( not Trait )
    {
        return false;
    }

    // Copied out before anything below can `Add()` — a reference into the Expr
    // arena does not survive one (rules/ast-rewrite.md).
    const Volt::Frontend::Member Written = *Callee;
    const Volt::Core::SourceRange Loc    = Volt::Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) );
    Out                                  = TruthType( Context );

    if ( Expr.Args.Size() != 1 or Expr.BlockArg.IsValid() )
    {
        Context.Report( Loc, "'" + std::string{ Context.Ctx.Ast.Text( Written.Name ) } + "' takes exactly one argument" );
        FoldInto( Context, Id, Loc, false, Out );
        return true;
    }

    const Volt::MiddleEnd::TypeSystem::SemaTypeId Receiver = InferExpr( Context, Written.Object );
    const NominalId Base = Context.Ctx.Values.Has( Receiver ) ? Context.Ctx.Values.Get( Receiver ).Base : NominalId{};
    if ( not Base.IsValid() )
    {
        // A generic definition's own body: the receiver is `T`, which has no
        // answer *yet*. Leave the node exactly as written and type it as a
        // truth value — `TypeSystem::ReinstantiateBody` walks this same node
        // again once the arguments are concrete, with `Redirects` set, and
        // folds it there. An un-instantiated generic body is never emitted,
        // so no backend can meet the unfolded node.
        //
        // The argument survives with the Call, so it has to be marked deferred
        // for exactly the reason AstInvariant's own check exempts one: it is
        // written inside a generic definition and answered at instantiation.
        // Nothing here descends into it — a trait's operand is a type name or
        // a symbol, never a value to evaluate — so without this it would be
        // reported as an expression nobody typed.
        Context.Ctx.Values.MarkDeferred( Expr.Args[0] );
        return true;
    }

    const Volt::Frontend::ExprId Arg = Expr.Args[0];
    Volt::MiddleEnd::ConstEval::TraitSite Site{ .Types = Context.Ctx.Types, .Receiver = Base, .Operand = {}, .Name = {} };

    if ( ConstEval::OperandOf( *Trait ) == ConstEval::EOperandKind::Name )
    {
        const auto *Symbol = std::get_if<Volt::Frontend::SymbolLiteral>( &Context.Ctx.Ast.Expr( Arg ) );
        if ( Symbol == nullptr )
        {
            Context.Report( Loc, "'" + std::string{ Context.Ctx.Ast.Text( Written.Name ) } + "' expects a symbol, as in '." +
                                     std::string{ Context.Ctx.Ast.Text( Written.Name ) } + " :name'" );
            FoldInto( Context, Id, Loc, false, Out );
            return true;
        }
        // A SymbolLiteral's lexeme is interned from the `:` onward (Lexer.cpp
        // makes the token at the colon), and a member is named without one.
        const std::string_view Spelled = Context.Ctx.Ast.Text( Symbol->Name );
        Site.Name                      = Spelled.starts_with( ':' ) ? Spelled.substr( 1 ) : Spelled;
    }
    else
    {
        Site.Operand = OperandNominal( Context, Arg );
        if ( not Site.Operand.IsValid() )
        {
            Context.Report( Loc, "'" + std::string{ Context.Ctx.Ast.Text( Written.Name ) } + "' expects a type name" );
            FoldInto( Context, Id, Loc, false, Out );
            return true;
        }
    }

    FoldInto( Context, Id, Loc, ConstEval::EvaluateTrait( *Trait, Site ), Out );
    return true;
}

// `super` — the method being walked, resolved one level up the chain instead
// of on `self`, and recorded on `Id` so the wrapping Call checks its arguments
// against the parent's signature and the backend emits a direct call to it.
//
// The name comes from `Context.CurrentMethodName`, never from a spelling here:
// `super` inside `def process` means `process`, and the old hardcoded
// "initialize" made every override's `super( … )` resolve to the parent's
// *constructor* — which type-checked only by accident when the arities happened
// to agree, and called the wrong function when they did.
//
// Static, not virtual: `super` names one specific body, which is exactly what
// makes it the one polymorphic-looking call this backend can already emit.
[[nodiscard]] Volt::MiddleEnd::TypeSystem::SemaTypeId
SuperType ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context, Volt::Frontend::ExprId Id )
{
    using namespace Volt::MiddleEnd::Analysis;
    using namespace Volt::MiddleEnd::TypeSystem;

    if ( not Context.SelfValue.IsValid() or not Context.CurrentMethodName.IsValid() )
    {
        return SemaTypeId{};
    }

    const SemaType &SelfConc = Context.Ctx.Values.Get( Context.SelfValue );
    if ( not SelfConc.Base.IsValid() )
    {
        return SemaTypeId{};
    }

    const NominalType &NomType = Context.Ctx.Types.Type( SelfConc.Base );
    if ( not NomType.Super.IsValid() )
    {
        return SemaTypeId{};
    }

    const SemaTypeId SuperInstance =
        Instantiate( Context.Ctx.Types, NomType.Super, SelfConc.Args, Context.SelfValue, Context.Ctx.Values );
    const Resolution Found = LookupOn( Context, SuperInstance, Context.Ctx.Ast.Text( Context.CurrentMethodName ) );
    if ( Found.Decl == nullptr )
    {
        return SemaTypeId{};
    }

    Context.CalleeResolution[Id.Value] = Found;
    return Found.Result;
}

} // namespace

/**
 * Public
 */

Volt::MiddleEnd::TypeSystem::SemaTypeId Volt::MiddleEnd::Analysis::InferExpr ( TypeCheckerContext &Context, Frontend::ExprId Id )
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

Volt::MiddleEnd::TypeSystem::SemaTypeId Volt::MiddleEnd::Analysis::ComputeExpr ( TypeCheckerContext &Context,
                                                                                 Frontend::ExprId Id )
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
            [&] ( const Frontend::SuperExpr & ) -> SemaTypeId { return SuperType( Context, Id ); },
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
                    return Context.MakeType( *Named, PlaceholderTypeArgs( Context.Ctx.Types, *Named ) );
                }

                // `super` reaches the parser as a keyword in some positions and
                // as a bare identifier in others; both mean the one thing, so
                // both go through the same resolution.
                if ( Context.Ctx.Ast.Text( Expr.Name ) == "super" and Context.SelfValue.IsValid() )
                {
                    if ( const SemaTypeId Super = SuperType( Context, Id ); Context.CalleeResolution.contains( Id.Value ) )
                    {
                        return Super;
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
                        // The implicit-`self` half of the visibility check.
                        // Writing the receiver down is what routes an access
                        // through MemberType; a bare name resolves here and
                        // would otherwise be the one way to reach a private
                        // member of a base class from a subclass.
                        CheckMemberAccess( Context, Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ), Found );
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
                    // `Volt::Core::AppConfig` — a type declared inside the module.
                    // TypeBinder hoists nested types to the top level, so the
                    // qualified spelling names the same nominal the bare one
                    // does; naked, exactly like a bare type name.
                    if ( const auto Named = Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Expr.Name ) ) )
                    {
                        Context.NakedTypeExprs.insert( Id.Value );
                        return Context.MakeType( *Named, PlaceholderTypeArgs( Context.Ctx.Types, *Named ) );
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
                UnitSink Sink = Context.MakeSink();
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
            [&] ( const Frontend::TypeTrait &Expr ) -> SemaTypeId
            {
                // Identical shape to `SizeOf` just above, and for the
                // identical reason: the operand names a type, so nothing is
                // descended into and the *resolved* type is published on this
                // node's own site rather than left as a spelling for someone
                // downstream to re-resolve. Inside a generic body it resolves
                // to nothing yet — `MiddleEnd::TypeSystem::ReinstantiateBody` runs this same
                // arm again once the arguments are concrete, exactly as it
                // does for `sizeof T` in `Pointer<T>#malloc`.
                //
                // The answer is a truth value, so the type is whatever claims
                // the `BoolLiteral` node kind — a node-kind claim, the
                // compiler's own vocabulary, never a Volt type name
                // (rules/zero-hardcode.md). Unlike a byte count it takes no
                // width from its use site: there is only one.
                UnitSink Sink = Context.MakeSink();
                Context.Ctx.Values.SetSiteType( BindingSite{ Id }, ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types,
                                                                                    Context.Generics(), Sink, Expr.Type ) );
                const auto Base = Context.Ctx.Types.LookupNodeKind( "BoolLiteral" );
                if ( not Base )
                {
                    return SemaTypeId{};
                }
                return Context.MakeType( *Base, {} );
            },
            [&] ( const Frontend::FuncAddr & ) -> SemaTypeId
            {
                // Inert like SizeOf/GenericInst: Target names an
                // already-resolved Method Decl, not a value expression, so
                // there is nothing to descend into. Always types as the
                // exact type `Proc<R>#code` was declared with — read off
                // that already-resolved field's own signature rather than
                // reconstructed from a byte-width node-kind claim, the same
                // trick LowerStringLit uses to borrow `String#initialize`'s
                // parameter type instead of re-deriving "UInt8" itself
                // (`rules/zero-hardcode.md`: no Volt type name spelled here).
                const auto FuncBase = Context.Ctx.Types.LookupNodeKind( "FuncType" );
                if ( not FuncBase )
                {
                    return SemaTypeId{};
                }
                const auto CodeField = Context.Ctx.Types.LookupMember( *FuncBase, "code" );
                if ( CodeField.Decl == nullptr )
                {
                    return SemaTypeId{};
                }
                return Instantiate( Context.Ctx.Types, CodeField.Decl->Result, {}, SemaTypeId{}, Context.Ctx.Values );
            },
            [&] ( const Frontend::Assign &Expr ) -> SemaTypeId
            {
                const Volt::MiddleEnd::TypeSystem::SemaTypeId Value = InferExpr( Context, Expr.Value );
                // A first write that *declares* the local — `result = 1`, no
                // annotation, no prior binding — records the initialiser as
                // provisional below. Pinning it afterwards would defeat that
                // record, so the two are mutually exclusive.
                bool bProvisional = false;
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
                            bProvisional                                       = true;
                        }
                        Context.UninitializedLocals.erase( Target->Name );
                    }
                }
                // AssignLowering (order 24) has already turned `x op= v` into
                // `x = x op v`, so compound assignment reaches this one check
                // with no case of its own.
                const SemaTypeId TargetType = InferExpr( Context, Expr.Target );
                // Constraining a provisional initialiser here would pin it to a
                // type read back off the local we just wrote *from that very
                // initialiser* — Int32 for `result = 1`. That is a no-op on the
                // type and a loss everywhere else: ConstrainExprType consumes
                // the UnconstrainedLiterals entry, and the literal is then
                // unreachable when the local finally settles. `result *= base`
                // moved the binding site to Int8 while the literal kept Int32,
                // and the backend stored an i32 into the i8 slot — three bytes
                // past its end. The last word has to win, so the first word
                // must not be spoken.
                if ( not bProvisional )
                {
                    Context.ConstrainExprType( Expr.Value, TargetType );
                }
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
                const Volt::Core::SourceRange Loc = Expr.Loc;
                Frontend::ExprId Exception        = Expr.Exception;

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

                // `raise "msg"` desugars to `Exception.new("msg")`. Detected
                // by *type*, not by the pre-lowering node kind: InterpLowering
                // (order 26) runs before TypeChecker (order 30), so an
                // interpolated message never survives to this point as an
                // `Interp` node — it is already a `Binary`/`Call` chain typed
                // `String`. The type claiming @[Literal( StringLiteral )] is
                // the same node-kind lookup TypeCompat uses for `nil`.
                const auto Root          = Context.Ctx.Types.LookupNodeKind( "RaiseExpr" );
                SemaTypeId ExceptionType = InferExpr( Context, Exception );
                NominalId Nominal =
                    Context.Ctx.Values.Has( ExceptionType ) ? Context.Ctx.Values.Get( ExceptionType ).Base : NominalId{};

                if ( Root.has_value() and Nominal.IsValid() and not IsSubclassOf( Context.Ctx.Types, Nominal, *Root ) )
                {
                    if ( const auto StringKind = Context.Ctx.Types.LookupNodeKind( "StringLiteral" );
                         StringKind.has_value() and Nominal == *StringKind )
                    {
                        Exception     = MakeExceptionConstructor( Context, Loc, Exception );
                        ExceptionType = InferExpr( Context, Exception );
                        Nominal =
                            Context.Ctx.Values.Has( ExceptionType ) ? Context.Ctx.Values.Get( ExceptionType ).Base : NominalId{};
                    }
                }

                std::get<Frontend::RaiseExpr>( Context.Ctx.Ast.Expr( Id ) ).Exception = Exception;
                if ( Root.has_value() and Nominal.IsValid() and not IsSubclassOf( Context.Ctx.Types, Nominal, *Root ) )
                {
                    Context.Report( Loc, "exception class/object expected" );
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
                        UnitSink Sink = Context.MakeSink();
                        ExceptionType =
                            ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Clause.ExceptionType );
                        const NominalId Nominal =
                            Context.Ctx.Values.Has( ExceptionType ) ? Context.Ctx.Values.Get( ExceptionType ).Base : NominalId{};
                        if ( const auto ExcOpt = Context.Ctx.Types.LookupNodeKind( "RaiseExpr" );
                             ExcOpt.has_value() and Nominal.IsValid() )
                        {
                            if ( not IsSubclassOf( Context.Ctx.Types, Nominal, *ExcOpt ) )
                            {
                                Context.Report( Frontend::LocOf( Context.Ctx.Ast.Stmt( ClauseId ) ),
                                                "rescue filter type must be a subclass of Exception" );
                            }
                        }
                    }
                    else if ( const auto ExcOpt = Context.Ctx.Types.LookupNodeKind( "RaiseExpr" ); ExcOpt.has_value() )
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
            // `if` is an expression — `val = if c ... else ... end` — so its
            // type is the join of the branches' trailing values, computed
            // exactly as CaseExpr's is. TrailingType also *walks* each branch,
            // which is what types the statements inside it.
            //
            // A branch that produces nothing (an `if` with no `else`, or one
            // whose body ends in an assignment) contributes an invalid type,
            // and UnifyBranchTypes lets the other side stand. That is the
            // right answer in statement position, where the enclosing ExprStmt
            // discards the value anyway.
            [&] ( const Frontend::If &Expr ) -> SemaTypeId
            {
                static_cast<void>( InferExpr( Context, Expr.Cond ) );
                const SemaTypeId Then = TrailingType( Context, Expr.Then );
                const SemaTypeId Else = TrailingType( Context, Expr.Else );
                return Context.UnifyBranchTypes( Then, Else );
            },
            [&] ( const Frontend::Call &Expr ) -> SemaTypeId { return CallType( Context, Id, Expr ); },
            [&] ( const Frontend::GenericInst &Expr ) -> SemaTypeId { return GenericInstType( Context, Id, Expr ); },
            // No Frontend::DotCall branch: DotCallLowering (order 23) rewrites
            // every one into `self.method( ... )` before TypeChecker (order 30)
            // runs, and RunPasses always runs the Lowering passes first. The
            // node reaching here would be a bug the AstInvariant pass reports.
            [&] ( const Frontend::Lambda &Expr ) -> SemaTypeId
            {
                const Volt::Core::SmallVec<SemaTypeId, 2> ParamTypes = BindClosureParams( Context, Expr.Params );
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
                const Volt::Core::SmallVec<SemaTypeId, 2> ParamTypes = BindClosureParams( Context, Expr.Params );
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
                UnitSink Sink = Context.MakeSink();
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

Volt::MiddleEnd::TypeSystem::SemaTypeId
Volt::MiddleEnd::Analysis::CallType ( TypeCheckerContext &Context, Frontend::ExprId Id, const Frontend::Call &Expr )
{
    // Before anything else, and before the callee is inferred: a receiver
    // trait is not a call, and resolving its callee as one would report an
    // unknown member for a name no type is meant to declare.
    if ( SemaTypeId Folded; FoldReceiverTrait( Context, Id, Expr, Folded ) )
    {
        return Folded;
    }

    if ( IsLambdaExpr( Context.Ctx.Ast, Expr.Callee ) and not Expr.Args.IsEmpty() and not Context.ExpectedClosure.IsValid() )
    {
        Volt::Core::SmallVec<SemaTypeId, 2> ArgTypes;
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
    // holding a closure is called as `f( x )`, which invokes the contract of
    // the type claiming FuncType. Without this, `f( x )` evaluated to `f`
    // itself, and every point-free pipeline stayed typed as its own Proc.
    auto It = Context.CalleeResolution.find( Expr.Callee.Value );
    if ( It == Context.CalleeResolution.end() )
    {
        const Resolution Applied = LookupCallOn( Context, Callee );
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

    // A getter field reached through explicit call syntax (`value.data()`)
    // is the same read as the paren-less `value.data` — a `getter` field
    // never synthesizes a real `Method` (TypeBinder records it as a plain
    // `EMemberKind::Field` Member), so nothing downstream could ever define
    // the symbol a genuine call would need. Rewritten away here, before any
    // of the Method-shaped logic below (arg binding, block typing) runs —
    // "no sugar survives lowering" (rules/core-ast.md) applies just as much
    // to a Call this shape as it does to Section/Composition, so a backend
    // never has to learn a second way a `Call` node can mean "read a place."
    if ( Found.Decl != nullptr and Found.Decl->Kind == EMemberKind::Field )
    {
        if ( not Expr.Args.IsEmpty() or Expr.BlockArg.IsValid() )
        {
            Context.Report( Expr.Loc,
                            "field '" + std::string{ Context.Ctx.Types.Text( Found.Decl->Name ) } + "' is not callable" );
        }
        Context.Ctx.Ast.Expr( Id ) = Context.Ctx.Ast.Expr( Expr.Callee );
        return Found.Result;
    }

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

Volt::MiddleEnd::TypeSystem::SemaTypeId
Volt::MiddleEnd::Analysis::GenericInstType ( TypeCheckerContext &Context, Frontend::ExprId Id, const Frontend::GenericInst &Expr )
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

    Volt::Core::SmallVec<SemaTypeId, 2> Args;
    for ( const Frontend::TypeId Arg : Expr.Args )
    {
        UnitSink Sink = Context.MakeSink();
        Args.PushBack( ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Arg ) );
    }

    CheckArity( Context, Expr.Loc, Nominal, Args.Size() );
    return Context.MakeType( Nominal, std::move( Args ) );
}
