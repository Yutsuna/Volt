#include "Volt/MiddleEnd/Lowering/LoweringPasses.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"

#include "DeclStmtWalker.hpp"
#include "ExprInferencer.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{

using namespace Volt;

// Whether *any* case of the enum `Base` names carries a payload — the
// discriminator that actually decides an enum's representation
// (`TypeBinder::EnsureEnumLayout`, `InstanceLayout::Of`): `Optional::None`
// has zero payload params on itself, but `Optional` as a whole is an
// `Aggregate` (because `Some` does), so `None` must still be *constructed*
// like `Some` — never collapsed to a bare constant the way a genuinely
// payload-less enum's cases are (`Color::Red`).
[[nodiscard]] bool EnumHasAnyPayload ( const Volt::MiddleEnd::TypeSystem::TypeStore &Store,
                                       Volt::MiddleEnd::TypeSystem::NominalId Base )
{
    if ( not Base.IsValid() )
    {
        return false;
    }
    return std::ranges::any_of(
        Store.Type( Base ).Members, [] ( const MiddleEnd::TypeSystem::Member &Entry )
        { return Entry.Kind == MiddleEnd::TypeSystem::EMemberKind::EnumCase and Entry.Params.Size() > 0; } );
}

// Same naming rule as `TypeBinder::EnsureEnumLayout` / `InstanceLayout::Of`
// (kept in sync by hand across all four sites now, since none of them can
// see the others' source): `$` + the case name alone for a single payload,
// `$<CaseName>_<Index>` past that. The `$` is load-bearing, not cosmetic —
// an `EnumCase` member of the bare case name already lives in the same
// `TypeStore::Members` vector (`Optional`'s `Some` case *and* its payload
// field would otherwise both be named "Some"), and `OwnMember`/
// `LookupMemberOn` return the first name match: without the prefix,
// `self.Some` silently resolved to the case's own self-constructing
// signature instead of the field, well-typed but with the wrong value —
// caught by a payload access returning a raw `ptr` where the function's
// declared return type was a scalar (module verification failure).
[[nodiscard]] std::string PayloadFieldName ( std::string_view CaseName, std::size_t Index, std::size_t Count )
{
    if ( Count <= 1 )
    {
        return "$" + std::string{ CaseName };
    }
    return "$" + std::string{ CaseName } + "_" + std::to_string( Index );
}

// `Color::Red` / `TaskStatus::InProgress`: no payload, nothing to
// construct — `self` never exists at runtime for these, only the ordinal
// does. Replaces the bare `Member` at `Id` with a plain `IntLiteral`; the
// expression's own SemaTypeId (already settled by ordinary inference)
// stays untouched, since only the AST *shape* was wrong.
void LowerEnumCaseConstant ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context, Frontend::ExprId Id, std::int64_t Ordinal )
{
    Frontend::AstContext &Ast         = Context.Ctx.Ast;
    const Volt::Core::SourceRange Loc = Frontend::LocOf( Ast.Expr( Id ) );
    Ast.Expr( Id ) =
        Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( std::to_string( Ordinal ) ) } };
}

// `Optional::Some( x )`: rewrites the `Call` at `Id` in place into `tmp =
// T.new(); tmp.tag = ordinal; tmp.<CaseName> = x; tmp` (a `BeginExpr`) —
// the exact shape `LowerArrayLit` (`LiteralLowering.cpp`) uses for `tmp =
// T.new(); tmp << e0; ...`. `Args` is a caller-owned copy
// (rules/ast-rewrite.md). Each field write's target is stamped directly
// with its value's already-known type rather than resolved through
// `MemberType`/`LookupOn` — there is no `Field`-kind `Member` for `tag` or
// a payload slot in the `TypeStore` (only a *layout* field, from Task 1),
// so an ordinary member lookup would never find one; `InferExpr`'s
// memoization (it returns immediately once `Values.ExprType` is set) means
// the `WalkStmt` pass below never attempts that lookup at all.
void LowerEnumCaseConstruction ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                                 Frontend::ExprId Id,
                                 Frontend::ExprList Args,
                                 std::string_view CaseName,
                                 std::int64_t Ordinal,
                                 MiddleEnd::TypeSystem::SemaTypeId ResultType )
{
    using namespace Volt::MiddleEnd::Analysis;

    if ( not ResultType.IsValid() or not Context.Ctx.Values.Has( ResultType ) )
    {
        return;
    }

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const MiddleEnd::TypeSystem::NominalId Base = Context.Ctx.Values.Get( ResultType ).Base;
    const std::string_view NameText             = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Base ).Name );
    const Frontend::Symbol NameSym              = Ast.Strings().Intern( NameText );

    const Frontend::ExprId ObjectId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = NameSym } } );
    Context.Ctx.Values.SetExprType( ObjectId, ResultType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId =
        Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );
    const Frontend::ExprId CtorId = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = {}, .Callee = NewMemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );

    const Frontend::Symbol TmpName   = Ast.MakeUniqueSymbol( "__enum_case" );
    const Frontend::ExprId TmpTarget = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    const Frontend::ExprId AssignId =
        Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = {}, .Target = TmpTarget, .Value = CtorId } } );

    MiddleEnd::Resolver::ScopeId CurrentScope = Context.Ctx.Scopes.ScopeOfExpr( Id );
    if ( not CurrentScope.IsValid() )
    {
        CurrentScope = MiddleEnd::Resolver::ScopeId{ 0 };
    }
    Context.Ctx.Scopes.Declare( CurrentScope, TmpName, TmpTarget );
    const MiddleEnd::Resolver::Binding *Bound = Context.Ctx.Scopes.Resolve( CurrentScope, TmpName );
    if ( Bound != nullptr )
    {
        Context.Ctx.Scopes.BindUse( TmpTarget, *Bound, false );
    }

    Frontend::StmtList Body;
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = AssignId } } ) );

    // tmp.tag = <ordinal>
    {
        const Frontend::ExprId TmpUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
        if ( Bound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( TmpUse, *Bound, true );
        }
        // Pre-stamping the enclosing Member below (TagFieldId) makes
        // InferExpr's memoization skip it entirely when WalkStmt later
        // reaches the Assign — which means it never recurses into this
        // Object child the way ordinary MemberType resolution would. Stamp
        // it directly, or `tmp` here is left with no type at all.
        Context.Ctx.Values.SetExprType( TmpUse, ResultType );
        const Frontend::ExprId TagFieldId = Ast.Add(
            Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = TmpUse, .Name = Ast.Strings().Intern( "tag" ) } } );
        const Frontend::ExprId TagValueId = Ast.Add(
            Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = {}, .Raw = Ast.Strings().Intern( std::to_string( Ordinal ) ) } } );
        Context.Ctx.Values.SetExprType( TagFieldId, InferExpr( Context, TagValueId ) );
        const Frontend::ExprId TagAssignId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = {}, .Target = TagFieldId, .Value = TagValueId } } );
        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = TagAssignId } } ) );
    }

    // tmp.<CaseName[_Index]> = arg, one per payload argument.
    for ( std::size_t Index = 0; Index < Args.Size(); ++Index )
    {
        const Frontend::ExprId TmpUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
        if ( Bound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( TmpUse, *Bound, true );
        }
        // Same reason as the `tag` block above: FieldId's pre-stamp below
        // would otherwise leave this Object child untyped.
        Context.Ctx.Values.SetExprType( TmpUse, ResultType );
        const std::string FieldName    = PayloadFieldName( CaseName, Index, Args.Size() );
        const Frontend::ExprId FieldId = Ast.Add(
            Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = TmpUse, .Name = Ast.Strings().Intern( FieldName ) } } );
        Context.Ctx.Values.SetExprType( FieldId, Context.Ctx.Values.ExprType( Args[Index] ) );
        const Frontend::ExprId FieldAssignId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = {}, .Target = FieldId, .Value = Args[Index] } } );
        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = FieldAssignId } } ) );
    }

    const Frontend::ExprId TrailingUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    if ( Bound != nullptr )
    {
        Context.Ctx.Scopes.BindUse( TrailingUse, *Bound, true );
    }
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = TrailingUse } } ) );

    // Type every synthesized statement off the local `Body` copy — never off
    // a re-read of the arena slot at `Id`, which this rewrite's own `Add()`
    // calls could reallocate out from under a reference (rules/ast-rewrite.md).
    for ( const Frontend::StmtId StmtId : Body )
    {
        WalkStmt( Context, StmtId );
    }

    Ast.Expr( Id ) =
        Frontend::ExprNode{ Frontend::BeginExpr{ .Loc = {}, .Body = std::move( Body ), .RescueClauses = {}, .EnsureBody = {} } };
}

void BindPatternUseStmt ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                          Frontend::StmtId Id,
                          Frontend::Symbol Name,
                          const MiddleEnd::Resolver::Binding &NewBinding,
                          MiddleEnd::TypeSystem::SemaTypeId FieldType,
                          bool bDeferred );

// Recursive descent mirroring `ClosureLifting.cpp`'s `RewriteCaptureUses` —
// same copy-out-children-before-recursing shape (`Meta::ForEachField`,
// reading only, rules/ast-rewrite.md) — but matching an unbound `Identifier`
// by *name* rather than against an existing `ScopeTable` binding: the
// clause body's own `val` was walked once by `ScopeResolver` (order 10),
// long before this pattern shape existed, and left unbound.
void BindPatternUse ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                      Frontend::ExprId Id,
                      Frontend::Symbol Name,
                      const MiddleEnd::Resolver::Binding &NewBinding,
                      MiddleEnd::TypeSystem::SemaTypeId FieldType,
                      bool bDeferred )
{
    if ( not Id.IsValid() )
    {
        return;
    }
    if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
    {
        return;
    }

    if ( const auto *Ident = std::get_if<Frontend::Identifier>( &Context.Ctx.Ast.Expr( Id ) ) )
    {
        if ( Ident->Name == Name and Context.Ctx.Scopes.BindingOf( Id ) == nullptr )
        {
            Context.Ctx.Scopes.BindUse( Id, NewBinding, true );
            Context.Ctx.Values.SetExprType( Id, FieldType );
            // Same reason as the declaration site in LowerEnumPatterns:
            // FieldType may be invalid inside a generic body's own
            // unsubstituted walk, and AstInvariant needs this marked
            // deferred rather than flagged as genuinely untyped.
            if ( bDeferred )
            {
                Context.Ctx.Values.MarkDeferred( Id );
            }
        }
        return;
    }

    std::vector<Frontend::ExprId> ChildExprs;
    std::vector<Frontend::StmtId> ChildStmts;
    std::visit(
        [&] ( auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Meta::ForEachField( Node,
                                    [&] ( const char *, auto &Field )
                                    {
                                        using F = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                        {
                                            ChildExprs.push_back( Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                ChildExprs.push_back( Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            ChildStmts.push_back( Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                ChildStmts.push_back( Child );
                                            }
                                        }
                                    } );
            }
        },
        Context.Ctx.Ast.Expr( Id ) );

    for ( const Frontend::ExprId Child : ChildExprs )
    {
        BindPatternUse( Context, Child, Name, NewBinding, FieldType, bDeferred );
    }
    for ( const Frontend::StmtId Child : ChildStmts )
    {
        BindPatternUseStmt( Context, Child, Name, NewBinding, FieldType, bDeferred );
    }
}

void BindPatternUseStmt ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                          Frontend::StmtId Id,
                          Frontend::Symbol Name,
                          const MiddleEnd::Resolver::Binding &NewBinding,
                          MiddleEnd::TypeSystem::SemaTypeId FieldType,
                          bool bDeferred )
{
    if ( not Id.IsValid() )
    {
        return;
    }

    std::vector<Frontend::ExprId> ChildExprs;
    std::vector<Frontend::StmtId> ChildStmts;
    std::visit(
        [&] ( auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Meta::ForEachField( Node,
                                    [&] ( const char *, auto &Field )
                                    {
                                        using F = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                        {
                                            ChildExprs.push_back( Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                ChildExprs.push_back( Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            ChildStmts.push_back( Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                ChildStmts.push_back( Child );
                                            }
                                        }
                                    } );
            }
        },
        Context.Ctx.Ast.Stmt( Id ) );

    for ( const Frontend::ExprId Child : ChildExprs )
    {
        BindPatternUse( Context, Child, Name, NewBinding, FieldType, bDeferred );
    }
    for ( const Frontend::StmtId Child : ChildStmts )
    {
        BindPatternUseStmt( Context, Child, Name, NewBinding, FieldType, bDeferred );
    }
}

} // namespace

void Volt::MiddleEnd::Lowering::LowerEnumCases ( TypeCheckerContext &Context )
{
    // Bound before the first Add() — every node this rewrite creates lands
    // past OriginalCount and is never itself resolved to an EnumCase, so
    // skipping it is deliberate (rules/ast-rewrite.md).
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();

    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }

        // A `case self when .Some(val)` *pattern* is, after CaseLowering's
        // desugar, syntactically identical to a genuine construction call
        // (`Call{Callee:Member(self,"Some")}`) and resolves through the
        // exact same `LookupOn` path — `CaseType` (ExprInferencer.cpp)
        // infers every pattern uniformly, construction or not. Inside a
        // generic definition's own (unsubstituted) body, such an expression
        // is marked deferred (`InferExpr`), and its receiver's own type
        // args are placeholders, not concrete — reading them here produced
        // an empty MonoRequest FlatArgs downstream and a "field has no
        // resolved layout" failure at monomorphization time. Skip deferred
        // expressions outright: `LowerEnumPatterns` (run before this sweep,
        // see TypeChecker.cpp) already rewrites every pattern — including
        // ones inside a generic body — into a plain tag comparison, so a
        // deferred `EnumCase`-resolving expression reaching this point is
        // never a pattern in disguise, only ever a genuine construction
        // this sweep must not act on before the body is instantiated
        // (rules/core-ast.md's "generic definition bodies" contract).
        if ( Context.Ctx.Values.IsDeferred( Id ) )
        {
            continue;
        }

        const Frontend::ExprNode &Node = Context.Ctx.Ast.Expr( Id );

        // A bare `Member` resolving to a *payload-carrying* case (this
        // specific case, e.g. `Some`) is always the callee sub-node of some
        // enclosing `Call` — there is no argument list reachable from here,
        // so it is left alone; the enclosing `Call` (below) is what carries
        // the arguments to rewrite. A payload-less case (`None`, `Red`)
        // reads as a bare Member with no wrapping Call and is handled here
        // directly — as a plain constant only if the *enum* has no payload
        // anywhere; otherwise (`Optional::None`) it still needs a real
        // construction, with zero payload fields written.
        if ( std::holds_alternative<Frontend::Member>( Node ) )
        {
            const auto Entry = Context.CalleeResolution.find( Id.Value );
            if ( Entry != Context.CalleeResolution.end() and Entry->second.Decl != nullptr and
                 Entry->second.Decl->Kind == EMemberKind::EnumCase and Entry->second.Decl->Params.Size() == 0 )
            {
                const MiddleEnd::TypeSystem::NominalId Base = Context.Ctx.Values.Has( Entry->second.Receiver )
                                                                  ? Context.Ctx.Values.Get( Entry->second.Receiver ).Base
                                                                  : MiddleEnd::TypeSystem::NominalId{};
                if ( EnumHasAnyPayload( Context.Ctx.Types, Base ) )
                {
                    const std::string CaseName  = std::string{ Context.Ctx.Types.Text( Entry->second.Decl->Name ) };
                    const std::int64_t Ordinal  = Entry->second.Decl->EnumOrdinal;
                    const SemaTypeId ResultType = Context.Ctx.Values.ExprType( Id );
                    LowerEnumCaseConstruction( Context, Id, {}, CaseName, Ordinal, ResultType );
                }
                else
                {
                    LowerEnumCaseConstant( Context, Id, Entry->second.Decl->EnumOrdinal );
                }
            }
            continue;
        }

        const auto *CallNode = std::get_if<Frontend::Call>( &Node );
        if ( CallNode == nullptr )
        {
            continue;
        }

        const auto Entry = Context.CalleeResolution.find( CallNode->Callee.Value );
        if ( Entry == Context.CalleeResolution.end() or Entry->second.Decl == nullptr or
             Entry->second.Decl->Kind != EMemberKind::EnumCase or Entry->second.Decl->Params.Size() == 0 )
        {
            continue;
        }

        // Copied out before Add() (rules/ast-rewrite.md): CallNode/Entry
        // point into arenas this rewrite's own Add() calls may reallocate.
        const Frontend::ExprList Args = CallNode->Args;
        const std::string CaseName    = std::string{ Context.Ctx.Types.Text( Entry->second.Decl->Name ) };
        const std::int64_t Ordinal    = Entry->second.Decl->EnumOrdinal;
        const SemaTypeId ResultType   = Context.Ctx.Values.ExprType( Id );

        LowerEnumCaseConstruction( Context, Id, Args, CaseName, Ordinal, ResultType );
    }
}

void Volt::MiddleEnd::Lowering::LowerEnumPatterns ( TypeCheckerContext &Context )
{
    // Bound before the first Add() — every node this rewrite creates lands
    // past OriginalCount and is never itself a CaseExpr, so skipping it is
    // deliberate (rules/ast-rewrite.md).
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();

    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::CaseExpr>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        // Copied out before Add() (rules/ast-rewrite.md): the Clauses list
        // itself never changes (each WhenClause's own slot is rewritten in
        // place by StmtId, never reordered or replaced), so the CaseExpr
        // node itself needs no write-back — only a plain read here.
        const Frontend::StmtList Clauses = std::get<Frontend::CaseExpr>( Context.Ctx.Ast.Expr( Id ) ).Clauses;

        for ( const Frontend::StmtId ClauseId : Clauses )
        {
            if ( not ClauseId.IsValid() or
                 Frontend::KindOf( Context.Ctx.Ast.Stmt( ClauseId ) ) != Frontend::StmtKind::WhenClause )
            {
                continue;
            }

            Frontend::WhenClause Clause = std::get<Frontend::WhenClause>( Context.Ctx.Ast.Stmt( ClauseId ) );

            Frontend::ExprList NewPatterns;
            Frontend::StmtList Prefix;
            bool bChanged = false;

            for ( const Frontend::ExprId PatternId : Clause.Patterns )
            {
                const auto *CallNode = std::get_if<Frontend::Call>( &Context.Ctx.Ast.Expr( PatternId ) );
                const Frontend::Member *MemberNode =
                    CallNode != nullptr ? std::get_if<Frontend::Member>( &Context.Ctx.Ast.Expr( CallNode->Callee ) ) : nullptr;
                // A payload-binding pattern (`.Some(val)`) carries exactly
                // one bare-identifier argument; a payload-less one (`.Todo`,
                // `.None`) carries none — CaseLowering's DotCall desugar
                // produces both shapes as a zero/one-arg `Call`, never
                // anything else a pattern here needs to recognize.
                const Frontend::Identifier *BindingIdent =
                    ( CallNode != nullptr and CallNode->Args.Size() == 1 )
                        ? std::get_if<Frontend::Identifier>( &Context.Ctx.Ast.Expr( CallNode->Args[0] ) )
                        : nullptr;
                const bool bZeroArgShape = CallNode != nullptr and CallNode->Args.Size() == 0;
                const bool bOneArgShape  = CallNode != nullptr and CallNode->Args.Size() == 1 and BindingIdent != nullptr;

                if ( CallNode == nullptr or MemberNode == nullptr or not( bZeroArgShape or bOneArgShape ) )
                {
                    NewPatterns.PushBack( PatternId );
                    continue;
                }

                // Copied out before any Add() below — CallNode/MemberNode/
                // BindingIdent are raw pointers into the Expr arena this
                // rewrite's own Add() calls may reallocate (rules/ast-rewrite.md).
                const Frontend::ExprId ScrutineeObjectId = MemberNode->Object;
                const Frontend::Symbol CaseNameSym       = MemberNode->Name;
                const Frontend::ExprId BindTarget        = bOneArgShape ? CallNode->Args[0] : Frontend::ExprId{};
                const Frontend::Symbol BindingNameSym    = bOneArgShape ? BindingIdent->Name : Frontend::Symbol{};

                // The case name, its ordinal, and whether it takes a payload
                // are per-nominal facts, never per-instantiation — reading
                // them off the scrutinee's *Base* alone (never its Args)
                // is what lets this run unconditionally, even inside a
                // generic definition's own (unsubstituted) body.
                const SemaTypeId ScrutineeType              = Context.Ctx.Values.ExprType( ScrutineeObjectId );
                const MiddleEnd::TypeSystem::NominalId Base = ScrutineeType.IsValid() and Context.Ctx.Values.Has( ScrutineeType )
                                                                  ? Context.Ctx.Values.Get( ScrutineeType ).Base
                                                                  : MiddleEnd::TypeSystem::NominalId{};
                const MiddleEnd::TypeSystem::Member *CaseMember =
                    Base.IsValid() ? Context.Ctx.Types.OwnMember( Base, Context.Ctx.Ast.Text( CaseNameSym ) ) : nullptr;

                const bool bWantsPayload = bOneArgShape;
                if ( CaseMember == nullptr or CaseMember->Kind != EMemberKind::EnumCase or
                     ( bWantsPayload and CaseMember->Params.Size() != 1 ) or
                     ( not bWantsPayload and CaseMember->Params.Size() != 0 ) )
                {
                    NewPatterns.PushBack( PatternId );
                    continue;
                }

                // Whether this pattern lives inside a generic definition's
                // own (unsubstituted) body: `Context.bGenericBody` is a
                // transient walk-position flag, already back to false by
                // the time this post-walk sweep runs regardless of where
                // `PatternId` actually sits — `IsDeferred`, set correctly
                // on it by the *original* CallType walk, is the per-node
                // fact that survives.
                const bool bInGenericBody = Context.Ctx.Values.IsDeferred( PatternId );

                // Copied out before Add(): CaseMember is a pointer into the
                // TypeStore's own Member vector, a *different* arena from
                // the Expr one — harmless with respect to the Expr Add()s
                // below, but Instantiate() below is what actually needs the
                // ordinal and the payload SigTypeId, so both are read now.
                const std::int64_t Ordinal = CaseMember->EnumOrdinal;
                const MiddleEnd::TypeSystem::SigTypeId PayloadSigType =
                    bWantsPayload ? CaseMember->Params[0] : MiddleEnd::TypeSystem::SigTypeId{};
                const bool bScrutineeHasPayload = EnumHasAnyPayload( Context.Ctx.Types, Base );

                bChanged = true;

                Frontend::AstContext &Ast = Context.Ctx.Ast;

                // pattern -> target.tag === ordinal (a payload-bearing
                // enum, `Optional::None`) or target === ordinal directly (a
                // fully payload-less enum, `TaskStatus::Todo` — `self` *is*
                // the ordinal, per `EnsureEnumLayout`'s Primitive layout).
                // The identical `===` desugar shape CaseLowering already
                // gives a non-DotCall pattern, so IsMachineOperatorOn/
                // Instructions.inl need no enum-specific case: it is typed
                // and emitted exactly like `when TaskStatus::InProgress`
                // already is.
                const Frontend::ExprId CompareLhsId =
                    bScrutineeHasPayload ? Ast.Add( Frontend::ExprNode{ Frontend::Member{
                                               .Loc = {}, .Object = ScrutineeObjectId, .Name = Ast.Strings().Intern( "tag" ) } } )
                                         : ScrutineeObjectId;
                const Frontend::ExprId OrdinalId = Ast.Add( Frontend::ExprNode{
                    Frontend::IntLiteral{ .Loc = {}, .Raw = Ast.Strings().Intern( std::to_string( Ordinal ) ) } } );
                if ( bScrutineeHasPayload )
                {
                    Context.Ctx.Values.SetExprType( CompareLhsId, InferExpr( Context, OrdinalId ) );
                }
                else
                {
                    static_cast<void>( InferExpr( Context, OrdinalId ) );
                }
                const Frontend::ExprId CompareId = Ast.Add( Frontend::ExprNode{
                    Frontend::Binary{ .Loc = {}, .Op = Frontend::TokenKind::TripleEq, .Lhs = CompareLhsId, .Rhs = OrdinalId } } );
                static_cast<void>( InferExpr( Context, CompareId ) );
                NewPatterns.PushBack( CompareId );

                if ( not bWantsPayload )
                {
                    continue;
                }

                // val = target.$<CaseName> — the payload field's type is
                // genuinely per-instantiation (T is concrete only after
                // substitution), so it is read the same way an ordinary
                // member access inside a generic body already is:
                // Instantiate() against the scrutinee's own (possibly still
                // deferred) Args. `$`-prefixed field name — see
                // `PayloadFieldName`'s comment: a bare `CaseNameSym` would
                // collide with the `EnumCase` member of the same name.
                const Volt::Core::SmallVec<SemaTypeId, 2> ScrutineeArgs = Context.Ctx.Values.Get( ScrutineeType ).Args;
                const Frontend::Symbol PayloadFieldSym =
                    Ast.Strings().Intern( "$" + std::string{ Context.Ctx.Ast.Text( CaseNameSym ) } );
                const Frontend::ExprId FieldReadId = Ast.Add(
                    Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = ScrutineeObjectId, .Name = PayloadFieldSym } } );
                const SemaTypeId FieldType = Instantiate(
                    Context.Ctx.Types, PayloadSigType, std::span<const SemaTypeId>{ ScrutineeArgs.begin(), ScrutineeArgs.Size() },
                    ScrutineeType, Context.Ctx.Values );
                Context.Ctx.Values.SetExprType( FieldReadId, FieldType );
                // Inside a generic definition's own (unsubstituted) body,
                // `T` has no SemaTypeId and Instantiate() can return an
                // invalid one — the same "typed only after substitution"
                // contract every other expression in such a body already
                // follows (rules/core-ast.md), and the same flag ordinary
                // InferExpr sets unconditionally whenever bGenericBody is
                // true. Without it, AstInvariant (order 40) reports this
                // node as a genuine untyped value expression instead of
                // recognizing it as deferred-until-instantiation.
                if ( bInGenericBody )
                {
                    Context.Ctx.Values.MarkDeferred( FieldReadId );
                }

                // The binding's declaration site is the pattern's own `val`
                // occurrence (BindTarget) — no fresh Identifier needed, and
                // it is what every other use in the clause body binds
                // against below.
                MiddleEnd::Resolver::ScopeId CurrentScope = Context.Ctx.Scopes.ScopeOfExpr( BindTarget );
                if ( not CurrentScope.IsValid() )
                {
                    CurrentScope = MiddleEnd::Resolver::ScopeId{ 0 };
                }
                Context.Ctx.Scopes.Declare( CurrentScope, BindingNameSym, BindTarget );
                const MiddleEnd::Resolver::Binding *Bound = Context.Ctx.Scopes.Resolve( CurrentScope, BindingNameSym );
                if ( Bound != nullptr )
                {
                    Context.Ctx.Scopes.BindUse( BindTarget, *Bound, false );
                }
                Context.Ctx.Values.SetExprType( BindTarget, FieldType );
                if ( bInGenericBody )
                {
                    Context.Ctx.Values.MarkDeferred( BindTarget );
                }

                const Frontend::ExprId BindAssignId =
                    Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = {}, .Target = BindTarget, .Value = FieldReadId } } );
                Prefix.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = BindAssignId } } ) );

                if ( Bound != nullptr )
                {
                    for ( const Frontend::StmtId BodyStmtId : Clause.Body )
                    {
                        BindPatternUseStmt( Context, BodyStmtId, BindingNameSym, *Bound, FieldType, bInGenericBody );
                    }
                }
            }

            if ( not bChanged )
            {
                continue;
            }

            Clause.Patterns = std::move( NewPatterns );

            Frontend::StmtList NewBody;
            for ( const Frontend::StmtId PrefixStmt : Prefix )
            {
                NewBody.PushBack( PrefixStmt );
            }
            for ( const Frontend::StmtId OriginalStmt : Clause.Body )
            {
                NewBody.PushBack( OriginalStmt );
            }
            Clause.Body = std::move( NewBody );

            for ( const Frontend::StmtId PrefixStmt : Prefix )
            {
                WalkStmt( Context, PrefixStmt );
            }

            Context.Ctx.Ast.Stmt( ClauseId ) = Frontend::StmtNode{ std::move( Clause ) };
        }
    }
}
