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
[[nodiscard]] bool ContainsExitStmt ( const Frontend::AstContext &Ast, Frontend::StmtId Id );
[[nodiscard]] bool ContainsExitExpr ( const Frontend::AstContext &Ast, Frontend::ExprId Id );

// A read-only structural descent (never a write, so no arena-rewrite hazard —
// rules/ast-rewrite.md's checklist is about mutation, not this kind of scan)
// over every Expr/Stmt reachable from Id, looking for a `Return`/`Break`/
// `Next` anywhere. Used only as the Phase 4 safety net (see
// ContainsUnstructuredExit below) — the structural recursion in ProcessBlock
// handles every statement-position If/While/CaseExpr/BeginExpr itself; this
// scan exists to catch the shapes it does not (an exit hiding inside an
// expression-position control construct, e.g. `x = if c then return 1 else 2
// end`) and bail rather than silently miss a finalize.
template <typename NodeVariant> bool ScanExitFields ( const Frontend::AstContext &Ast, const NodeVariant &Variant )
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
                                            bFound = bFound or ContainsExitExpr( Ast, Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                bFound = bFound or ContainsExitExpr( Ast, Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            bFound = bFound or ContainsExitStmt( Ast, Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                bFound = bFound or ContainsExitStmt( Ast, Child );
                                            }
                                        }
                                    } );
            }
        },
        Variant );
    return bFound;
}

bool ContainsExitStmt ( const Frontend::AstContext &Ast, Frontend::StmtId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    const Frontend::StmtNode &Node = Ast.Stmt( Id );
    if ( std::holds_alternative<Frontend::Return>( Node ) or std::holds_alternative<Frontend::Break>( Node ) or
         std::holds_alternative<Frontend::Next>( Node ) )
    {
        return true;
    }
    return ScanExitFields( Ast, Node );
}

bool ContainsExitExpr ( const Frontend::AstContext &Ast, Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    return ScanExitFields( Ast, Ast.Expr( Id ) );
}

// Phase 4 safety net: ProcessBlock recurses structurally into every
// statement-position If/While/CaseExpr/BeginExpr (the four StmtList-bearing
// constructs — Return/Break/Next can *only* ever live inside a StmtList, so
// this set is structurally exhaustive for anything reached this way). What
// it does not reach is a Return/Break/Next hiding inside an
// expression-position occurrence of one of those four (`x = if c then
// return 1 else 2 end`, a Call argument, ...) — rare in practice, and
// refused the same way Phase 1/3 refused whole classes of methods: leave the
// method completely untouched rather than risk missing a finalize.
[[nodiscard]] bool ContainsUnstructuredExit ( const Frontend::AstContext &Ast, const Frontend::StmtList &Body )
{
    for ( const Frontend::StmtId Id : Body )
    {
        if ( not Id.IsValid() )
        {
            continue;
        }
        const Frontend::StmtNode &Node = Ast.Stmt( Id );

        // A top-level Return/Break/Next is exactly what ProcessBlock's
        // splice step handles directly — not "unstructured".
        if ( std::holds_alternative<Frontend::Return>( Node ) or std::holds_alternative<Frontend::Break>( Node ) or
             std::holds_alternative<Frontend::Next>( Node ) )
        {
            continue;
        }

        if ( const auto *WhileNode = std::get_if<Frontend::While>( &Node ) )
        {
            if ( ContainsUnstructuredExit( Ast, WhileNode->Body ) )
            {
                return true;
            }
            continue;
        }

        if ( const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Node ) )
        {
            const Frontend::ExprNode &Inner = Ast.Expr( ExprStmtNode->Expr );
            if ( const auto *IfNode = std::get_if<Frontend::If>( &Inner ) )
            {
                if ( ContainsUnstructuredExit( Ast, IfNode->Then ) or ContainsUnstructuredExit( Ast, IfNode->Else ) )
                {
                    return true;
                }
                continue;
            }
            if ( const auto *CaseNode = std::get_if<Frontend::CaseExpr>( &Inner ) )
            {
                for ( const Frontend::StmtId ClauseId : CaseNode->Clauses )
                {
                    const auto &Clause = std::get<Frontend::WhenClause>( Ast.Stmt( ClauseId ) );
                    if ( ContainsUnstructuredExit( Ast, Clause.Body ) )
                    {
                        return true;
                    }
                }
                if ( ContainsUnstructuredExit( Ast, CaseNode->ElseBody ) )
                {
                    return true;
                }
                continue;
            }
            if ( const auto *BeginNode = std::get_if<Frontend::BeginExpr>( &Inner ) )
            {
                if ( ContainsUnstructuredExit( Ast, BeginNode->Body ) )
                {
                    return true;
                }
                for ( const Frontend::StmtId RescueId : BeginNode->RescueClauses )
                {
                    const auto &Rescue = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
                    if ( ContainsUnstructuredExit( Ast, Rescue.Body ) )
                    {
                        return true;
                    }
                }
                if ( ContainsUnstructuredExit( Ast, BeginNode->EnsureBody ) )
                {
                    return true;
                }
                continue;
            }
            // Anything else (Assign, a bare Call, ...) — fall back to the
            // exhaustive scan: it may still hide an exit inside an
            // expression-position If/CaseExpr/BeginExpr.
            if ( ContainsExitExpr( Ast, ExprStmtNode->Expr ) )
            {
                return true;
            }
            continue;
        }

        if ( ContainsExitStmt( Ast, Id ) )
        {
            return true;
        }
    }
    return false;
}

// One finalize candidate, gathered from the Method scope's own Order/
// Bindings (UnusedChecker.cpp's own enumeration shape) — a local whose site
// is *directly* one of a given Body's own top-level statements. BodyIndex is
// that statement's position within THAT Body, so reverse-declaration order
// is a plain sort. ProcessBlock calls CollectCandidates once per StmtList it
// visits (Method::Body, and every nested If/While/CaseExpr/BeginExpr body),
// so a candidate declared inside a branch is only ever claimed by that
// branch's own body, never by an enclosing one.
struct FinalizeCandidate
{
    Sema::BindingSite Site;
    Core::Symbol Name;
    std::size_t BodyIndex = 0;
};

// Candidacy per the plan (design decision 2): declared directly at the top
// level of Body (never hoisted in from a nested branch — an implicit
// local's binding is hoisted to NearestNonBranchScope by ScopeResolver even
// when its initializing Assign sits inside a nested If/While/CaseExpr/
// BeginExpr, so walking Scope.Bindings alone is not enough; the extra "is
// this site literally one of Body's own elements" check below is what
// sidesteps that hoisting subtlety without new liveness analysis — and is
// exactly why this same function, called with a *nested* Body, still only
// picks up that Body's own locals: MethodScope.Order lists every local in
// the whole method flat, but TopLevel*Index below is built fresh from
// whichever Body was passed), Aggregate-layout typed, and declaring a
// `FinalizeName` member.
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
        // statement of THIS Body, are never candidates here.
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

// candidates declared strictly before BodyPos in Candidates, in reverse
// declaration order (last-declared first) — the shared shape both the
// ambient-building recursion and the local splice loop need.
[[nodiscard]] std::vector<FinalizeCandidate> ReversedBefore ( const std::vector<FinalizeCandidate> &Candidates, std::size_t Pos )
{
    std::vector<FinalizeCandidate> Out;
    for ( auto It = Candidates.rbegin(); It != Candidates.rend(); ++It )
    {
        if ( It->BodyIndex < Pos )
        {
            Out.push_back( *It );
        }
    }
    return Out;
}

// ProcessBlock applies the wrap-and-splice transform to one StmtList, then
// recurses into every nested one. Two "ambient" lists carry candidates
// belonging to *enclosing* StmtLists, already in finalize order (nearest
// declared first) — needed because a `return`/`break`/`next` nested several
// levels deep must finalize not just its own immediate Body's candidates,
// but every enclosing scope's candidates it unwinds past too:
//
// - ReturnAmbient accumulates across every enclosing level without limit —
//   a `return` unwinds the whole method, loops included.
// - LoopAmbient resets to empty each time recursion enters a fresh
//   `While::Body` — a `break`/`next` only unwinds as far as its own
//   innermost enclosing loop.
[[nodiscard]] Frontend::StmtList ProcessBlock ( TypeCheckerContext &Context,
                                                Sema::ScopeId MethodScope,
                                                Frontend::StmtList Body,
                                                bool bInLoop,
                                                std::vector<FinalizeCandidate> ReturnAmbient,
                                                std::vector<FinalizeCandidate> LoopAmbient )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Candidates local to THIS Body only (CollectCandidates rebuilds its
    // top-level index from whichever Body is passed, so a nested branch's
    // own locals are never claimed by an outer level) — computed before
    // recursing so each child call can be given the right ambient slice.
    const std::vector<FinalizeCandidate> Candidates = CollectCandidates( Context, MethodScope, Body );

    // Step 1 — recurse bottom-up into every nested StmtList reachable
    // through a statement-position If/While/CaseExpr/BeginExpr, mutating
    // arena entries in place. Body's own StmtId sequence does not change
    // here: only the *contents* of the nested nodes it points at do, via
    // copy-out/Add()/write-back (rules/ast-rewrite.md) — Body[i] itself
    // still names the same slot afterward.
    for ( std::size_t Pos = 0; Pos < Body.Size(); ++Pos )
    {
        const Frontend::StmtId Id = Body[Pos];
        if ( not Id.IsValid() )
        {
            continue;
        }

        if ( std::holds_alternative<Frontend::While>( Ast.Stmt( Id ) ) )
        {
            // Copied out before ProcessBlock's own Add()s.
            Frontend::While WhileCopy = std::get<Frontend::While>( Ast.Stmt( Id ) );

            std::vector<FinalizeCandidate> ChildReturnAmbient = ReversedBefore( Candidates, Pos );
            ChildReturnAmbient.insert( ChildReturnAmbient.end(), ReturnAmbient.begin(), ReturnAmbient.end() );

            WhileCopy.Body = ProcessBlock( Context, MethodScope, std::move( WhileCopy.Body ), true,
                                           std::move( ChildReturnAmbient ), /*fresh loop*/ {} );
            Ast.Stmt( Id ) = Frontend::StmtNode{ std::move( WhileCopy ) };
            continue;
        }

        const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Id ) );
        if ( ExprStmtNode == nullptr )
        {
            continue;
        }
        const Frontend::ExprId InnerId = ExprStmtNode->Expr;

        if ( std::holds_alternative<Frontend::If>( Ast.Expr( InnerId ) ) )
        {
            Frontend::If IfCopy = std::get<Frontend::If>( Ast.Expr( InnerId ) );

            std::vector<FinalizeCandidate> ChildReturnAmbient = ReversedBefore( Candidates, Pos );
            ChildReturnAmbient.insert( ChildReturnAmbient.end(), ReturnAmbient.begin(), ReturnAmbient.end() );
            std::vector<FinalizeCandidate> ChildLoopAmbient;
            if ( bInLoop )
            {
                ChildLoopAmbient = ReversedBefore( Candidates, Pos );
                ChildLoopAmbient.insert( ChildLoopAmbient.end(), LoopAmbient.begin(), LoopAmbient.end() );
            }

            IfCopy.Then =
                ProcessBlock( Context, MethodScope, std::move( IfCopy.Then ), bInLoop, ChildReturnAmbient, ChildLoopAmbient );
            IfCopy.Else = ProcessBlock( Context, MethodScope, std::move( IfCopy.Else ), bInLoop, std::move( ChildReturnAmbient ),
                                        std::move( ChildLoopAmbient ) );
            Ast.Expr( InnerId ) = Frontend::ExprNode{ std::move( IfCopy ) };
        }
        else if ( std::holds_alternative<Frontend::CaseExpr>( Ast.Expr( InnerId ) ) )
        {
            Frontend::CaseExpr CaseCopy = std::get<Frontend::CaseExpr>( Ast.Expr( InnerId ) );

            std::vector<FinalizeCandidate> ChildReturnAmbient = ReversedBefore( Candidates, Pos );
            ChildReturnAmbient.insert( ChildReturnAmbient.end(), ReturnAmbient.begin(), ReturnAmbient.end() );
            std::vector<FinalizeCandidate> ChildLoopAmbient;
            if ( bInLoop )
            {
                ChildLoopAmbient = ReversedBefore( Candidates, Pos );
                ChildLoopAmbient.insert( ChildLoopAmbient.end(), LoopAmbient.begin(), LoopAmbient.end() );
            }

            for ( const Frontend::StmtId ClauseId : CaseCopy.Clauses )
            {
                Frontend::WhenClause ClauseCopy = std::get<Frontend::WhenClause>( Ast.Stmt( ClauseId ) );
                ClauseCopy.Body = ProcessBlock( Context, MethodScope, std::move( ClauseCopy.Body ), bInLoop, ChildReturnAmbient,
                                                ChildLoopAmbient );
                Ast.Stmt( ClauseId ) = Frontend::StmtNode{ std::move( ClauseCopy ) };
            }
            CaseCopy.ElseBody   = ProcessBlock( Context, MethodScope, std::move( CaseCopy.ElseBody ), bInLoop,
                                                std::move( ChildReturnAmbient ), std::move( ChildLoopAmbient ) );
            Ast.Expr( InnerId ) = Frontend::ExprNode{ std::move( CaseCopy ) };
        }
        else if ( std::holds_alternative<Frontend::BeginExpr>( Ast.Expr( InnerId ) ) )
        {
            Frontend::BeginExpr BeginCopy = std::get<Frontend::BeginExpr>( Ast.Expr( InnerId ) );

            std::vector<FinalizeCandidate> ChildReturnAmbient = ReversedBefore( Candidates, Pos );
            ChildReturnAmbient.insert( ChildReturnAmbient.end(), ReturnAmbient.begin(), ReturnAmbient.end() );
            std::vector<FinalizeCandidate> ChildLoopAmbient;
            if ( bInLoop )
            {
                ChildLoopAmbient = ReversedBefore( Candidates, Pos );
                ChildLoopAmbient.insert( ChildLoopAmbient.end(), LoopAmbient.begin(), LoopAmbient.end() );
            }

            BeginCopy.Body =
                ProcessBlock( Context, MethodScope, std::move( BeginCopy.Body ), bInLoop, ChildReturnAmbient, ChildLoopAmbient );
            for ( const Frontend::StmtId RescueId : BeginCopy.RescueClauses )
            {
                Frontend::RescueClause RescueCopy = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
                RescueCopy.Body = ProcessBlock( Context, MethodScope, std::move( RescueCopy.Body ), bInLoop, ChildReturnAmbient,
                                                ChildLoopAmbient );
                Ast.Stmt( RescueId ) = Frontend::StmtNode{ std::move( RescueCopy ) };
            }
            BeginCopy.EnsureBody = ProcessBlock( Context, MethodScope, std::move( BeginCopy.EnsureBody ), bInLoop,
                                                 std::move( ChildReturnAmbient ), std::move( ChildLoopAmbient ) );
            Ast.Expr( InnerId )  = Frontend::ExprNode{ std::move( BeginCopy ) };
        }
    }

    // Body may still need Step 3's splice even with zero LOCAL candidates:
    // an ambient candidate from an enclosing scope must still be finalized
    // at a Return/Break/Next that is a top-level element of THIS Body (a
    // bare `break` inside an `if` with no local of its own, unwinding past
    // the while body's own `x` — see ProcessBlock's own doc comment). Only
    // bail out completely when there is truly nothing to do anywhere.
    if ( Body.IsEmpty() or ( Candidates.empty() and ReturnAmbient.empty() and ( not bInLoop or LoopAmbient.empty() ) ) )
    {
        return Body;
    }

    // A SourceRange for the synthesized nodes — Body is non-empty here (the
    // guard above returned already otherwise).
    Core::SourceRange Loc;
    std::visit(
        [&] ( const auto &N )
        {
            using T = std::remove_cvref_t<decltype( N )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Loc = N.Loc;
            }
        },
        Ast.Stmt( Body[0] ) );

    // Step 3 — splice finalize calls directly before each top-level Return,
    // and — when this Body is a loop's own (bInLoop) — before each
    // top-level Break/Next too: all three bypass Ensure exactly the same
    // way (EmitReturn is a raw CreateRet; EmitBreak/EmitNext branch straight
    // to Frame.Loops.back().{Merge,Latch} — see StmtReturnBreakNext.cpp). A
    // non-local break/next (no enclosing loop in this frame — the closure
    // transport) is not spliced here: it already threads through EnsureBody
    // via the existing BreakFlagSlot/EmitPoisonedPath mechanism (wrap, Step
    // 5 below), same as an unhandled raise. Each spliced exit finalizes THIS
    // Body's own candidates before its position, then every ambient
    // candidate from enclosing scopes it is also unwinding past.
    Frontend::StmtList SplicedBody;
    for ( std::size_t BodyPos = 0; BodyPos < Body.Size(); ++BodyPos )
    {
        const Frontend::StmtId StmtId = Body[BodyPos];
        const bool bIsReturn          = StmtId.IsValid() and std::holds_alternative<Frontend::Return>( Ast.Stmt( StmtId ) );
        const bool bIsLoopExit        = bInLoop and StmtId.IsValid() and
                                 ( std::holds_alternative<Frontend::Break>( Ast.Stmt( StmtId ) ) or
                                   std::holds_alternative<Frontend::Next>( Ast.Stmt( StmtId ) ) );

        if ( bIsReturn or bIsLoopExit )
        {
            // Move-out exemption (design decision 4b) applies only to
            // `return x` — `break`/`next` carry no value inside a loop
            // (StmtReturnBreakNext.cpp refuses `break v`/`next v` there), so
            // there is nothing to move out. `x` may name a local of THIS
            // Body or of any enclosing one (`return a` from inside a nested
            // `if`, where `a` was declared in the method body) — the skip
            // below applies uniformly to both lists.
            Core::Symbol ReturnMovedOutName;
            if ( bIsReturn )
            {
                const Frontend::Return &ReturnRef = std::get<Frontend::Return>( Ast.Stmt( StmtId ) );
                if ( ReturnRef.Value.IsValid() )
                {
                    if ( const auto *ReturnIdentifier = std::get_if<Frontend::Identifier>( &Ast.Expr( ReturnRef.Value ) ) )
                    {
                        ReturnMovedOutName = ReturnIdentifier->Name;
                    }
                }
            }

            std::vector<FinalizeCandidate> ToFinalize     = ReversedBefore( Candidates, BodyPos );
            const std::vector<FinalizeCandidate> &Ambient = bIsReturn ? ReturnAmbient : LoopAmbient;
            ToFinalize.insert( ToFinalize.end(), Ambient.begin(), Ambient.end() );

            for ( const FinalizeCandidate &Candidate : ToFinalize )
            {
                if ( ReturnMovedOutName.IsValid() and Candidate.Name == ReturnMovedOutName )
                {
                    continue;
                }
                const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( Candidate.Site );
                const Frontend::ExprId CallId    = BuildFinalizeCall( Context, Loc, Candidate, LocalType, MethodScope );
                SplicedBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
            }
        }
        SplicedBody.PushBack( StmtId );
    }

    // Nothing declared directly in THIS Body — every exit spliced above was
    // finalizing purely ambient (enclosing-scope) candidates, so there is no
    // local fall-through/raise coverage to wrap either. Return the spliced
    // result as-is; an outer level (or the method's own top-level wrap)
    // owns the corresponding EnsureBody for whatever was just spliced in.
    if ( Candidates.empty() )
    {
        return SplicedBody;
    }

    // Step 4 — tail-type / move-out for THIS Body's own fall-through exit,
    // identical reasoning to the Method::Body case: wrapping Body in a
    // BeginExpr moves the original tail expression into the wrap's inner
    // Body, so the wrap's own ExprId needs that tail's resolved type copied
    // onto it for value convergence to work when this Body is itself used
    // as a value (an If/CaseExpr/BeginExpr branch feeding an assignment). A
    // purely statement-position body (an ordinary `if`/`while` used only for
    // control flow) never has this type read, so copying it is harmless.
    Sema::SemaTypeId TailType;
    Core::Symbol MovedOutName;
    if ( const auto *TailStmt = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Body.Size() - 1] ) ) )
    {
        TailType = Context.Ctx.Values.ExprType( TailStmt->Expr );
        if ( const auto *TailIdentifier = std::get_if<Frontend::Identifier>( &Ast.Expr( TailStmt->Expr ) ) )
        {
            MovedOutName = TailIdentifier->Name;
        }
    }

    // Step 5 — reverse declaration order, last-declared first-finalized,
    // covering fall-through / raise / non-local-break through the existing
    // EnsureBody unwind path (BeginRescueEmitter.cpp's EmitBegin).
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

    Frontend::StmtList Result;
    Result.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = WrapId } } ) );
    return Result;
}

// Cheapest possible whole-method check: does this method's scope contain
// even one Aggregate-layout, finalize-declaring binding anywhere (ignoring
// nesting depth)? If not, the method is left completely untouched without
// ever calling ProcessBlock — the silent default this feature must be for
// ordinary code.
[[nodiscard]] bool ScopeHasAnyFinalizeCandidate ( TypeCheckerContext &Context, Sema::ScopeId MethodScope )
{
    if ( not MethodScope.IsValid() )
    {
        return false;
    }
    const Sema::Scope &ScopeRef = Context.Ctx.Scopes.Get( MethodScope );
    for ( const Core::Symbol Name : ScopeRef.Order )
    {
        const auto BindingIt = ScopeRef.Bindings.find( Name );
        if ( BindingIt == ScopeRef.Bindings.end() )
        {
            continue;
        }
        const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( BindingIt->second.Site );
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
        if ( Sema::LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, LocalType, LocalType, FinalizeName ).Decl != nullptr )
        {
            return true;
        }
    }
    return false;
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

        // Phase 4: ProcessBlock recurses into every statement-position
        // If/While/CaseExpr/BeginExpr body, so a Return/Break/Next nested
        // inside one of those is handled directly at its own level. Only an
        // exit hiding in an expression-position control construct
        // (ContainsUnstructuredExit) still causes the whole method to be
        // skipped untouched.
        if ( ContainsUnstructuredExit( Ast, Node.Body ) )
        {
            continue;
        }

        const Sema::ScopeId MethodScope = Context.Ctx.Scopes.ScopeOf( Node.Body[0] );
        if ( not MethodScope.IsValid() )
        {
            continue;
        }

        if ( not ScopeHasAnyFinalizeCandidate( Context, MethodScope ) )
        {
            continue;
        }

        Node.Body      = ProcessBlock( Context, MethodScope, std::move( Node.Body ), /*bInLoop=*/false, {}, {} );
        Ast.Decl( Id ) = Frontend::DeclNode{ std::move( Node ) };
    }
}
