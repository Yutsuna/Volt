#include "FinalizeLowering.hpp"

#include "ExprInferencer.hpp"
#include "MemberResolver.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace
{

using namespace Volt;
using namespace Volt::Sema::TypeCheckerPass;

// Forward declarations — Stmt and Expr recurse into each other.
[[nodiscard]] bool ContainsReturnStmt ( const Frontend::AstContext &Ast, Frontend::StmtId Id );
[[nodiscard]] bool ContainsReturnExpr ( const Frontend::AstContext &Ast, Frontend::ExprId Id );

// A read-only structural descent (never a write, so no arena-rewrite hazard —
// rules/ast-rewrite.md's checklist is about mutation, not this kind of scan)
// over every Expr/Stmt reachable from Id, looking for a single `Return`
// anywhere — nested inside an `If`/`While`/`CaseExpr`/`BeginExpr`, or even a
// closure literal's own Body (a non-local return still exits *this* method).
// Phase 1 skips the whole method rather than risk missing one of these exits.
template <typename NodeVariant> bool ScanReturnFields ( const Frontend::AstContext &Ast, const NodeVariant &Variant )
{
    bool bFound = false;
    std::visit(
        [&] ( const auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Meta::ForEachField( Node,
                                    [&] ( const char *, const auto &Field )
                                    {
                                        if ( bFound )
                                        {
                                            return;
                                        }
                                        using F = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                        {
                                            bFound = bFound or ContainsReturnExpr( Ast, Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                bFound = bFound or ContainsReturnExpr( Ast, Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            bFound = bFound or ContainsReturnStmt( Ast, Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                bFound = bFound or ContainsReturnStmt( Ast, Child );
                                            }
                                        }
                                    } );
            }
        },
        Variant );
    return bFound;
}

bool ContainsReturnStmt ( const Frontend::AstContext &Ast, Frontend::StmtId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    if ( std::holds_alternative<Frontend::Return>( Ast.Stmt( Id ) ) )
    {
        return true;
    }
    return ScanReturnFields( Ast, Ast.Stmt( Id ) );
}

bool ContainsReturnExpr ( const Frontend::AstContext &Ast, Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    return ScanReturnFields( Ast, Ast.Expr( Id ) );
}

// Phase 3 gate: a `Return` nested inside a branch/loop/begin body (never a
// direct element of Body itself) is left for Phase 4 — the whole method is
// skipped untouched, same as Phase 1's blanket gate used to do for every
// Return. A `Return` that IS one of Body's own top-level elements is fine:
// the splice below handles it directly.
[[nodiscard]] bool ContainsNestedReturn ( const Frontend::AstContext &Ast, const Frontend::StmtList &Body )
{
    return std::ranges::any_of( Body,
                                [&Ast] ( const Frontend::StmtId Id )
                                {
                                    // a top-level Return itself is not "nested"
                                    return Id.IsValid() and not std::holds_alternative<Frontend::Return>( Ast.Stmt( Id ) ) and
                                           ContainsReturnStmt( Ast, Id );
                                } );
}

// One finalize candidate, gathered from the Method scope's own Order/
// Bindings (UnusedChecker.cpp's own enumeration shape) — a local whose site
// is *directly* one of Body's own top-level statements. BodyIndex is that
// statement's position, so reverse-declaration order is a plain sort.
struct FinalizeCandidate
{
    Sema::BindingSite Site;
    Core::Symbol Name;
    std::size_t BodyIndex = 0;
};

// Candidacy per the plan (design decision 2): declared directly at the top
// level of Body (never hoisted in from a nested branch — an implicit local's
// binding is hoisted to NearestNonBranchScope by ScopeResolver even when its
// initializing Assign sits inside a nested If/While/CaseExpr/BeginExpr, so
// walking Scope.Bindings alone is not enough; the extra "is this site
// literally one of Body's own elements" check below is what sidesteps that
// hoisting subtlety without new liveness analysis), Aggregate-layout typed,
// and declaring a `FinalizeName` member.
[[nodiscard]] std::vector<FinalizeCandidate>
CollectCandidates ( TypeCheckerContext &Context, Sema::ScopeId MethodScope, const Frontend::StmtList &Body )
{
    std::vector<FinalizeCandidate> Result;
    if ( not MethodScope.IsValid() )
    {
        return Result;
    }

    const Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Body's own top-level statements: a LocalDecl is its own site; a bare
    // `x = Type.new(...)` is an ExprStmt wrapping an Assign, whose Target is
    // the implicit local's site (ScopeTable.hpp's own doc comment on
    // BindingSite's ExprId arm).
    std::unordered_map<std::uint32_t, std::size_t> TopLevelStmtIndex;
    std::unordered_map<std::uint32_t, std::size_t> TopLevelImplicitIndex;
    for ( std::size_t Index = 0; Index < Body.Size(); ++Index )
    {
        const Frontend::StmtId StmtId = Body[Index];
        if ( not StmtId.IsValid() )
        {
            continue;
        }
        TopLevelStmtIndex[StmtId.Value] = Index;

        if ( const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( StmtId ) ) )
        {
            if ( const auto *AssignNode = std::get_if<Frontend::Assign>( &Ast.Expr( ExprStmtNode->Expr ) ) )
            {
                TopLevelImplicitIndex[AssignNode->Target.Value] = Index;
            }
        }
    }

    const Sema::Scope &ScopeRef = Context.Ctx.Scopes.Get( MethodScope );
    for ( const Core::Symbol Name : ScopeRef.Order )
    {
        const auto BindingIt = ScopeRef.Bindings.find( Name );
        if ( BindingIt == ScopeRef.Bindings.end() )
        {
            continue;
        }
        const Sema::Binding &Entry = BindingIt->second;

        std::size_t BodyIndex = 0;
        bool bTopLevel        = false;
        if ( const auto *StmtSite = std::get_if<Frontend::StmtId>( &Entry.Site ) )
        {
            if ( const auto Found = TopLevelStmtIndex.find( StmtSite->Value ); Found != TopLevelStmtIndex.end() )
            {
                bTopLevel = true;
                BodyIndex = Found->second;
            }
        }
        else if ( const auto *ExprSite = std::get_if<Frontend::ExprId>( &Entry.Site ) )
        {
            if ( const auto Found = TopLevelImplicitIndex.find( ExprSite->Value ); Found != TopLevelImplicitIndex.end() )
            {
                bTopLevel = true;
                BodyIndex = Found->second;
            }
        }
        // ParamId / DeclId sites, and anything not literally a top-level
        // statement of this Body, are never Phase 1 candidates.
        if ( not bTopLevel )
        {
            continue;
        }

        const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( Entry.Site );
        if ( not LocalType.IsValid() or not Context.Ctx.Values.Has( LocalType ) )
        {
            continue;
        }

        const Sema::NominalId Base = Context.Ctx.Values.Get( LocalType ).Base;
        if ( not Base.IsValid() )
        {
            continue;
        }
        const Sema::NominalType &Nominal = Context.Ctx.Types.Type( Base );
        if ( not Nominal.Layout.IsValid() or
             Sema::KindOf( Context.Ctx.Types.Get( Nominal.Layout ) ) != Sema::LayoutKind::Aggregate )
        {
            continue;
        }

        const Sema::InstantiatedMember FinalizeMember =
            Sema::LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, LocalType, LocalType, FinalizeName );
        if ( FinalizeMember.Decl == nullptr )
        {
            continue;
        }

        Result.push_back( FinalizeCandidate{ .Site = Entry.Site, .Name = Name, .BodyIndex = BodyIndex } );
    }

    std::ranges::sort( Result,
                       [] ( const FinalizeCandidate &A, const FinalizeCandidate &B ) { return A.BodyIndex < B.BodyIndex; } );
    return Result;
}

// `<candidate>.finalize()` — an ordinary read of the local, resolved through
// InferExpr/MemberType exactly as hand-written source would be
// (rules/backend-machine-only.md's "a synthesized operator is not a built-in
// either").
[[nodiscard]] Frontend::ExprId BuildFinalizeCall ( TypeCheckerContext &Context,
                                                   Core::SourceRange Loc,
                                                   const FinalizeCandidate &Candidate,
                                                   Sema::SemaTypeId LocalType,
                                                   Sema::ScopeId MethodScope )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const Frontend::ExprId ReadId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Candidate.Name } } );
    if ( const Sema::Binding *Bound = Context.Ctx.Scopes.Resolve( MethodScope, Candidate.Name ) )
    {
        Context.Ctx.Scopes.BindUse( ReadId, *Bound, true );
    }
    Context.Ctx.Values.SetExprType( ReadId, LocalType );

    const Frontend::ExprId MemberId = Ast.Add(
        Frontend::ExprNode{ Frontend::Member{ .Loc = Loc, .Object = ReadId, .Name = Ast.Strings().Intern( FinalizeName ) } } );
    const Frontend::ExprId CallId = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = Loc, .Callee = MemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );
    InferExpr( Context, CallId );
    return CallId;
}

} // namespace

void Volt::Sema::TypeCheckerPass::InsertFinalizeCalls ( TypeCheckerContext &Context )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Decls never grow in this pass (only Method's own Body slot is
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

        // Phase 4 TODO: a Return nested inside a branch/loop/begin body is
        // out of scope (same "top-level only" restriction CollectCandidates
        // already applies to locals) — left completely untouched. A
        // top-level Return is handled below by the splice.
        if ( ContainsNestedReturn( Ast, Node.Body ) )
        {
            continue;
        }

        const Sema::ScopeId MethodScope = Context.Ctx.Scopes.ScopeOf( Node.Body[0] );
        if ( not MethodScope.IsValid() )
        {
            continue;
        }

        // Cheapest possible check first: a method with no finalizable
        // candidate at all is left completely untouched — the silent
        // default this whole feature must be for ordinary code.
        const std::vector<FinalizeCandidate> Candidates = CollectCandidates( Context, MethodScope, Node.Body );
        if ( Candidates.empty() )
        {
            continue;
        }

        const Core::SourceRange Loc = Node.Loc;

        // Phase 3: splice finalize calls directly before each top-level
        // Return — Return bypasses Ensure entirely (EmitReturn is a raw
        // CreateRet, no ensure-stack lookup: see FinalizeLowering.hpp), so
        // wrap (a) below can never reach that exit path at all. Only
        // Return statements that are literally one of Body's own top-level
        // elements are spliced here — ContainsNestedReturn above already
        // refused the whole method if one hides inside a branch/loop/begin
        // body (Phase 4).
        Frontend::StmtList SplicedBody;
        for ( std::size_t BodyPos = 0; BodyPos < Node.Body.Size(); ++BodyPos )
        {
            const Frontend::StmtId StmtId = Node.Body[BodyPos];
            if ( StmtId.IsValid() and std::holds_alternative<Frontend::Return>( Ast.Stmt( StmtId ) ) )
            {
                // Copied out before any Add() below — rules/ast-rewrite.md:
                // Ast.Stmt(StmtId) is a reference into the Stmt arena, and
                // BuildFinalizeCall's own Add()s (a different arena, but the
                // discipline is copy-first regardless) must never be issued
                // while a live reference into an arena is held.
                const Frontend::Return ReturnCopy = std::get<Frontend::Return>( Ast.Stmt( StmtId ) );

                // Move-out exemption, per exit site (design decision 4b): a
                // bare `return x` handing back exactly one candidate does
                // not finalize that candidate at THIS splice point — its
                // buffer becomes the caller's own local.
                Core::Symbol ReturnMovedOutName;
                if ( ReturnCopy.Value.IsValid() )
                {
                    if ( const auto *ReturnIdentifier = std::get_if<Frontend::Identifier>( &Ast.Expr( ReturnCopy.Value ) ) )
                    {
                        ReturnMovedOutName = ReturnIdentifier->Name;
                    }
                }

                // Reverse declaration order, restricted to candidates
                // already declared strictly before this Return's own
                // position — anything declared later in program order is
                // simply not live on this path yet.
                for ( auto It = Candidates.rbegin(); It != Candidates.rend(); ++It )
                {
                    if ( It->BodyIndex >= BodyPos )
                    {
                        continue;
                    }
                    if ( ReturnMovedOutName.IsValid() and It->Name == ReturnMovedOutName )
                    {
                        continue;
                    }
                    const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( It->Site );
                    const Frontend::ExprId CallId    = BuildFinalizeCall( Context, Loc, *It, LocalType, MethodScope );
                    SplicedBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
                }
            }
            SplicedBody.PushBack( StmtId );
        }

        // `EmitStmts` marks Body's own last statement `bTail`, and — when
        // the method returns a value — reads that tail expression's value
        // through `EmitExpr` for the implicit `ret` (StmtEmitter.cpp); no
        // explicit `Return` node is involved. Wrapping the whole Body in a
        // BeginExpr moves the ORIGINAL tail into the wrap's own inner
        // Body, so `WrapId` — not the original expression — is what now
        // sits in that outer tail slot. EmitBegin only produces a
        // converged value when its own ExprId already carries a resolved
        // type (BeginRescueEmitter.cpp: `TypeOfExpr(Id)` gates the
        // convergence Slot); with none, `EmitBegin` returns nullptr and the
        // method silently falls off the end into DefineSweep's zero-fill,
        // not a crash — a wrong return value with no diagnostic. Copying
        // the original tail's already-resolved type (TypeChecker's
        // EnterMethod already ran ConstrainExprType against the method's
        // own return type, DeclStmtWalker.cpp) onto WrapId restores it.
        Sema::SemaTypeId TailType;
        Core::Symbol MovedOutName; // invalid unless the tail is a bare Identifier
        if ( const auto *TailStmt = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Node.Body[Node.Body.Size() - 1] ) ) )
        {
            TailType = Context.Ctx.Values.ExprType( TailStmt->Expr );

            // Move-out exemption (design decision 4a): Volt has no explicit
            // `return` in a straight-line, no-return method (Phase 1's own
            // gate already refused any method containing one) — the tail
            // statement itself IS the return, Ruby-style. A candidate whose
            // buffer this tail hands back by bare name must not be finalized
            // here, or the caller receives a use-after-free: Enumerable#map/
            // #filter/#to_array (source/Lib/Mixins/Enumerable.vl) are exactly
            // this shape — `result = Array<U>.new; each { ... }; result`.
            if ( const auto *TailIdentifier = std::get_if<Frontend::Identifier>( &Ast.Expr( TailStmt->Expr ) ) )
            {
                MovedOutName = TailIdentifier->Name;
            }
        }

        // Reverse declaration order — last-declared, first-finalized, like a
        // C++ destructor. The moved-out candidate (if any) is skipped here
        // only — its buffer becomes the caller's own local, finalized when
        // that scope in turn exits.
        Frontend::StmtList EnsureBody;
        for ( auto It = Candidates.rbegin(); It != Candidates.rend(); ++It )
        {
            if ( MovedOutName.IsValid() and It->Name == MovedOutName )
            {
                continue;
            }
            const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( It->Site );
            const Frontend::ExprId CallId    = BuildFinalizeCall( Context, Loc, *It, LocalType, MethodScope );
            EnsureBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
        }

        const Frontend::ExprId WrapId = Ast.Add( Frontend::ExprNode{ Frontend::BeginExpr{
            .Loc = Loc, .Body = std::move( SplicedBody ), .RescueClauses = {}, .EnsureBody = std::move( EnsureBody ) } } );
        if ( TailType.IsValid() )
        {
            Context.Ctx.Values.SetExprType( WrapId, TailType );
        }

        Frontend::StmtList NewBody;
        NewBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = WrapId } } ) );

        Node.Body      = std::move( NewBody );
        Ast.Decl( Id ) = Frontend::DeclNode{ std::move( Node ) };
    }
}
