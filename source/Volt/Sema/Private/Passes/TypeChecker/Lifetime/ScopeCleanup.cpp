#include "ScopeCleanup.hpp"

#include "../ExprInferencer.hpp"
#include "CleanupRegion.hpp"
#include "ExitPaths.hpp"
#include "ExprOwnership.hpp"
#include "FinalizeCallBuilder.hpp"
#include "Raii/Ownership.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"
#include "Volt/Sema/Raii/OwnershipInference.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Volt::Sema::TypeCheckerPass::Lifetime
{

namespace
{

    using namespace Volt;
    using namespace Volt::Sema::TypeCheckerPass;
    // The exit analysis (which paths leave a region, and what each hands back)
    // lives in Lifetime/ExitPaths.hpp — see that header for why it needs no
    // TypeCheckerContext at all.
    using namespace Volt::Sema::TypeCheckerPass::Lifetime;

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
        // Non-null only for a RescueClause's own bound variable (see
        // BuildRescueCandidate) — its Binding lives in a *different* Scope
        // (RescueClause pushes its own Branch scope for VarName, ScopeResolver.
        // cpp's own WalkStmt) than MethodScope, so BuildFinalizeCall's ordinary
        // `Scopes.Resolve( MethodScope, Name )` would never find it. Carrying
        // the already-resolved Binding directly (a stable reference into that
        // Scope's own Bindings map — never a temporary, since BindUse stores a
        // pointer to whatever Binding it is given) sidesteps needing a second,
        // scope-aware resolution path in BuildFinalizeCall.
        const Sema::Binding *DirectBinding = nullptr;
    };

    // Aggregate-layout typed and declaring a `FinalizeName` member — the
    // candidacy type-check shared by CollectCandidates (per ordinary local),
    // ScopeHasAnyFinalizeCandidate (the whole-method cheap pre-check), and
    // BuildRescueCandidate (a RescueClause's own bound variable).
    //
    // A thin binding onto `Raii::IsFinalizeCandidateType`: candidacy is a
    // question about a *type*, so it lives in Private/Raii/Ownership.hpp where
    // the Driver seam can ask it too, and this overload only unpacks the
    // PassContext this file happens to carry it in.
    [[nodiscard]] bool IsFinalizeCandidateType ( TypeCheckerContext &Context, Sema::SemaTypeId LocalType )
    {
        return Sema::Raii::IsFinalizeCandidateType( Context.Ctx.Types, Context.Ctx.Values, LocalType );
    }

    // Does `Candidate`'s own Binding appear, by bare name, as an argument the
    // callee *keeps* — anywhere in this file?
    //
    // Volt has no move/borrow system (the alias-init guard just above already
    // documents this same limitation for `a2 = a1`), so passing an Aggregate
    // local to a method is a struct copy exactly like an assignment is. When
    // the callee stores that copy — a constructor's `@x` shorthand, an
    // `Array<T>#push` writing it through `@buffer` — the callee's own cleanup
    // will free the identical pointer this local's would. Two independent
    // finalizes of one buffer is a double free (confirmed empirically:
    // `arr = Array<String>.new; arr << "x"; b = Bag.new( arr )`).
    //
    // Which arguments those are is no longer guessed. It used to be scoped to
    // constructors, because widening it to *every* call cost `WhileLoop.vl`'s
    // `pending_events` a leak: `drain_event_queue( events )` reads its
    // argument and returns, so the local was still live and still had to be
    // finalized. `Member::ParamEscapes` answers precisely that — derived per
    // parameter at the Driver seam (`Raii::InferParameterEscape`) — so the
    // scoping is gone and the narrower question is asked instead. A
    // construction still escapes unconditionally: `bConstructs` is certain
    // where the derived bit is only a proof attempt.
    //
    // `BindingOf` resolves each argument's own Binding, so a shadowed name in
    // a nested scope can never be confused with this candidate's.
    [[nodiscard]] bool EscapesAsCallArgument ( TypeCheckerContext &Context, const Sema::Binding &CandidateBinding )
    {
        const Frontend::AstContext &Ast = Context.Ctx.Ast;
        for ( std::size_t Index = 0; Index < Ast.ExprCount(); ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            const auto *CallNode = std::get_if<Frontend::Call>( &Ast.Expr( Id ) );
            if ( CallNode == nullptr )
            {
                continue;
            }
            const auto ResolutionIt = Context.CalleeResolution.find( CallNode->Callee.Value );
            const bool bConstructs  = ResolutionIt != Context.CalleeResolution.end() and ResolutionIt->second.bConstructs;
            const Member *Callee    = ResolutionIt != Context.CalleeResolution.end() ? ResolutionIt->second.Decl : nullptr;

            for ( std::size_t Arg = 0; Arg < CallNode->Args.Size(); ++Arg )
            {
                const Frontend::ExprId ArgId = CallNode->Args[Arg];
                if ( not ArgId.IsValid() or not std::holds_alternative<Frontend::Identifier>( Ast.Expr( ArgId ) ) )
                {
                    continue;
                }
                if ( Context.Ctx.Scopes.BindingOf( ArgId ) != &CandidateBinding )
                {
                    continue;
                }
                if ( bConstructs or Callee == nullptr or Sema::Raii::ParameterEscapes( *Callee, Arg ) )
                {
                    return true;
                }
            }
        }
        return false;
    }

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
        // The initializing expression behind each top-level implicit local — a
        // bare `x = Type.new(...)` Assign's own Value — needed below to refuse
        // candidacy for an aliasing copy (see the bAliasInit check).
        std::unordered_map<std::uint32_t, Frontend::ExprId> TopLevelImplicitInit;
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
                    TopLevelImplicitInit[AssignNode->Target.Value]  = AssignNode->Value;
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
            Frontend::ExprId InitId; // invalid unless this site is an implicit local
            if ( const auto *StmtSite = std::get_if<Frontend::StmtId>( &Entry.Site ) )
            {
                if ( const auto Found = TopLevelStmtIndex.find( StmtSite->Value ); Found != TopLevelStmtIndex.end() )
                {
                    bTopLevel = true;
                    BodyIndex = Found->second;
                    if ( const auto *LocalDeclNode = std::get_if<Frontend::LocalDecl>( &Ast.Stmt( *StmtSite ) ) )
                    {
                        InitId = LocalDeclNode->Init;
                    }
                }
            }
            else if ( const auto *ExprSite = std::get_if<Frontend::ExprId>( &Entry.Site ) )
            {
                if ( const auto Found = TopLevelImplicitIndex.find( ExprSite->Value ); Found != TopLevelImplicitIndex.end() )
                {
                    bTopLevel = true;
                    BodyIndex = Found->second;
                    InitId    = TopLevelImplicitInit.at( ExprSite->Value );
                }
            }
            // ParamId / DeclId sites, and anything not literally a top-level
            // statement of THIS Body, are never candidates here.
            if ( not bTopLevel )
            {
                continue;
            }

            // Volt has no move/borrow system (a documented, accepted limitation
            // — see the move-out exemption below, which already treats a bare
            // `return x` conservatively for the same reason): a local whose own
            // initializer *reads a place* is, by construction, never a fresh
            // value — it is a struct-copy *alias* of whatever that place holds
            // (Aggregate locals are copy semantics, not reference semantics —
            // rules/backend-machine-only.md via .agents/backend/abi.md).
            // Finalizing it here as well as the thing it aliases is a double
            // free, confirmed empirically (`a2 = a1; ...` on an `Array<T>`
            // crashes with "double free detected in tcache" without this
            // guard).
            //
            // The three node kinds that can read a place are exactly the three
            // that can also be a paren-less *invocation*
            // (Lifetime/ExprOwnership.hpp), so the test is not syntactic: it is
            // the same proof `Temporaries` demands of an unnamed value. `s =
            // d.full_id` keeps its candidacy because `full_id` was proven to
            // return owned; `s = d.serial` loses it because a getter hands back
            // a field the receiver still owns and whose cascade will free it.
            // A Call-initialized local (`x = f(...)`) is not filtered here at
            // all — an ordinary function returning an owned value is a
            // distinct, legitimate, already-tested shape (see ReturnMovedOut.vl).
            if ( InitId.IsValid() and
                 ( std::holds_alternative<Frontend::Identifier>( Ast.Expr( InitId ) ) or
                   std::holds_alternative<Frontend::InstanceVar>( Ast.Expr( InitId ) ) or
                   std::holds_alternative<Frontend::Member>( Ast.Expr( InitId ) ) ) and
                 not ProducesOwnedValue( Context, InitId ) )
            {
                continue;
            }

            const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( Entry.Site );
            if ( not IsFinalizeCandidateType( Context, LocalType ) )
            {
                continue;
            }

            if ( EscapesAsCallArgument( Context, Entry ) )
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

        auto MakeReadId = [&] () -> Frontend::ExprId
        {
            const Frontend::ExprId ReadId =
                Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Candidate.Name } } );
            if ( Candidate.DirectBinding != nullptr )
            {
                Context.Ctx.Scopes.BindUse( ReadId, *Candidate.DirectBinding, true );
            }
            else if ( const Sema::Binding *Bound = Context.Ctx.Scopes.Resolve( MethodScope, Candidate.Name ) )
            {
                Context.Ctx.Scopes.BindUse( ReadId, *Bound, true );
            }
            Context.Ctx.Values.SetExprType( ReadId, LocalType );
            return ReadId;
        };

        return BuildFinalizeCallOnReceiver( Context, Loc, MakeReadId, LocalType );
    }

    // candidates declared strictly before BodyPos in Candidates, in reverse
    // declaration order (last-declared first) — the shared shape both the
    // ambient-building recursion and the local splice loop need.
    [[nodiscard]] std::vector<FinalizeCandidate> ReversedBefore ( const std::vector<FinalizeCandidate> &Candidates,
                                                                  std::size_t Pos )
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

    // A RescueClause's own bound variable, as a finalize candidate for its own
    // Body — see FinalizeCandidate::DirectBinding's own doc comment for why this
    // needs the RescueClause's own Scope (never MethodScope) and a stable
    // Binding reference rather than a synthesized temporary. ScopeOf(RescueId)
    // resolves directly to that Scope — ScopeResolver.cpp's RescueClause arm
    // overrides the generic SetScopeOf(Id, Current) specifically so this (and
    // BackendLLVM's SlotFor, which classifies unit-scope vs frame-local storage
    // the same way) never has to special-case "the scope this Stmt pushed for
    // itself" via a linear scan. Returns nullopt for an anonymous clause
    // (`rescue` / `rescue; ...` with no bound name), a non-finalizable type, or a
    // Scope lookup that somehow fails. An *empty* Body is deliberately still a
    // candidate — `e` is bound and goes out of scope the moment the (empty)
    // clause ends, exactly like a C++ destructor firing for a variable whose
    // scope has no statements; ProcessBlock's own empty-Body path synthesizes
    // the one-statement finalize-only body this produces a candidate for.
    [[nodiscard]] std::optional<FinalizeCandidate>
    BuildRescueCandidate ( TypeCheckerContext &Context, const Frontend::RescueClause &Clause, Frontend::StmtId RescueId )
    {
        if ( not Clause.VarName.IsValid() )
        {
            return std::nullopt;
        }
        const Sema::ScopeId RescueScope = Context.Ctx.Scopes.ScopeOf( RescueId );
        if ( not RescueScope.IsValid() )
        {
            return std::nullopt;
        }
        const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( Sema::BindingSite{ RescueId } );
        if ( not IsFinalizeCandidateType( Context, LocalType ) )
        {
            return std::nullopt;
        }
        const Sema::Scope &RescueScopeRef = Context.Ctx.Scopes.Get( RescueScope );
        const auto BindingIt              = RescueScopeRef.Bindings.find( Clause.VarName );
        if ( BindingIt == RescueScopeRef.Bindings.end() )
        {
            return std::nullopt;
        }
        return FinalizeCandidate{
            .Site = Sema::BindingSite{ RescueId }, .Name = Clause.VarName, .BodyIndex = 0, .DirectBinding = &BindingIt->second };
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
    //
    // ImplicitCandidate is set only when Body is a RescueClause's own — its
    // bound variable (`rescue e : Exception`) is declared before Body's first
    // statement even runs, in a Scope of its own ScopeResolver pushes
    // specifically for it (never MethodScope, so CollectCandidates can never see
    // it), yet must still finalize at every one of Body's own exits exactly like
    // an ordinary local. It is folded into the local candidate list (finalized
    // after everything CollectCandidates itself found, since it was "declared"
    // before all of them) and threaded into ambient the same way any other
    // local of this Body would be, wherever this call recurses further inward.
    // EmptyBodyLoc is read only on the Body.IsEmpty() + ImplicitCandidate path
    // below (an empty RescueClause body has no Body[0] of its own to read a
    // SourceRange off of — the caller passes the RescueClause's own Loc instead).
    [[nodiscard]] Frontend::StmtList ProcessBlock ( TypeCheckerContext &Context,
                                                    Sema::ScopeId MethodScope,
                                                    Frontend::StmtList Body,
                                                    bool bInLoop,
                                                    std::vector<FinalizeCandidate> ReturnAmbient,
                                                    std::vector<FinalizeCandidate> LoopAmbient,
                                                    std::optional<FinalizeCandidate> ImplicitCandidate = std::nullopt,
                                                    Core::SourceRange EmptyBodyLoc                     = {} )
    {
        Frontend::AstContext &Ast = Context.Ctx.Ast;

        // Candidates local to THIS Body only (CollectCandidates rebuilds its
        // top-level index from whichever Body is passed, so a nested branch's
        // own locals are never claimed by an outer level) — computed before
        // recursing so each child call can be given the right ambient slice.
        // ImplicitCandidate is deliberately NOT folded in here: it is "declared"
        // before position 0, which BodyIndex (an unsigned, position-filtered
        // value real candidates use) cannot represent — every consumer below
        // appends it explicitly, after the position-filtered result, wherever
        // that consumer's own ordering calls for "finalized last".
        const std::vector<FinalizeCandidate> Candidates = CollectCandidates( Context, MethodScope, Body );

        // Step 1 — recurse bottom-up into every nested StmtList this Body
        // owns, mutating arena entries in place. Body's own StmtId sequence
        // does not change here: only the *contents* of the nested nodes it
        // points at do, via copy-out/Add()/write-back (rules/ast-rewrite.md)
        // — Body[i] itself still names the same slot afterward.
        //
        // "Nested" is answered by CollectNestedBlockExprs, i.e. by an
        // If/CaseExpr/BeginExpr occurring anywhere in the statement's
        // expressions — not only as an ExprStmt's own top-level expression.
        // That is the whole of Phase 5: `x = if c then return 1 else 2 end`
        // and `f( begin ... end )` reach the identical recursion an
        // ordinary statement-position `if` does, so a `return`/`break`/`next`
        // inside one is spliced exactly like any other. There is no
        // per-shape arm and no bail-out; the old `ContainsUnstructuredExit`
        // safety net, which refused such a method wholesale, is gone.
        //
        // `While` stays a case of its own only because it is a *statement*,
        // so no expression walk can reach it.
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
                if ( ImplicitCandidate.has_value() )
                {
                    ChildReturnAmbient.push_back( *ImplicitCandidate );
                }

                WhileCopy.Body = ProcessBlock( Context, MethodScope, std::move( WhileCopy.Body ), true,
                                               std::move( ChildReturnAmbient ), /*fresh loop*/ {} );
                Ast.Stmt( Id ) = Frontend::StmtNode{ std::move( WhileCopy ) };
                continue;
            }

            // Every nested block this statement owns, at any expression
            // depth. Collected by value before the first rewrite below: each
            // arm Add()s (rules/ast-rewrite.md), and the ids stay valid
            // because every rewrite writes back into the same slot.
            const std::vector<Frontend::ExprId> Nested = CollectNestedBlockExprs( Ast, Id );
            if ( Nested.empty() )
            {
                continue;
            }

            // Computed once for the statement rather than per arm: the three
            // arms below used three verbatim copies of this, which is also
            // why the whole statement shares one ambient slice — every block
            // it owns is entered at the same point in this Body's sequence.
            std::vector<FinalizeCandidate> StmtReturnAmbient = ReversedBefore( Candidates, Pos );
            StmtReturnAmbient.insert( StmtReturnAmbient.end(), ReturnAmbient.begin(), ReturnAmbient.end() );
            std::vector<FinalizeCandidate> StmtLoopAmbient;
            if ( bInLoop )
            {
                StmtLoopAmbient = ReversedBefore( Candidates, Pos );
                StmtLoopAmbient.insert( StmtLoopAmbient.end(), LoopAmbient.begin(), LoopAmbient.end() );
            }
            if ( ImplicitCandidate.has_value() )
            {
                StmtReturnAmbient.push_back( *ImplicitCandidate );
                if ( bInLoop )
                {
                    StmtLoopAmbient.push_back( *ImplicitCandidate );
                }
            }

            for ( const Frontend::ExprId InnerId : Nested )
            {
                Context.Ctx.Stats.RaiiNestedExpressionExits += 1;

                if ( std::holds_alternative<Frontend::If>( Ast.Expr( InnerId ) ) )
                {
                    Frontend::If IfCopy = std::get<Frontend::If>( Ast.Expr( InnerId ) );

                    std::vector<FinalizeCandidate> ChildReturnAmbient = StmtReturnAmbient;
                    std::vector<FinalizeCandidate> ChildLoopAmbient   = StmtLoopAmbient;

                    IfCopy.Then = ProcessBlock( Context, MethodScope, std::move( IfCopy.Then ), bInLoop, ChildReturnAmbient,
                                                ChildLoopAmbient );
                    IfCopy.Else = ProcessBlock( Context, MethodScope, std::move( IfCopy.Else ), bInLoop,
                                                std::move( ChildReturnAmbient ), std::move( ChildLoopAmbient ) );
                    Ast.Expr( InnerId ) = Frontend::ExprNode{ std::move( IfCopy ) };
                    continue;
                }

                if ( std::holds_alternative<Frontend::CaseExpr>( Ast.Expr( InnerId ) ) )
                {
                    Frontend::CaseExpr CaseCopy = std::get<Frontend::CaseExpr>( Ast.Expr( InnerId ) );

                    std::vector<FinalizeCandidate> ChildReturnAmbient = StmtReturnAmbient;
                    std::vector<FinalizeCandidate> ChildLoopAmbient   = StmtLoopAmbient;

                    for ( const Frontend::StmtId ClauseId : CaseCopy.Clauses )
                    {
                        Frontend::WhenClause ClauseCopy = std::get<Frontend::WhenClause>( Ast.Stmt( ClauseId ) );
                        ClauseCopy.Body      = ProcessBlock( Context, MethodScope, std::move( ClauseCopy.Body ), bInLoop,
                                                             ChildReturnAmbient, ChildLoopAmbient );
                        Ast.Stmt( ClauseId ) = Frontend::StmtNode{ std::move( ClauseCopy ) };
                    }
                    CaseCopy.ElseBody   = ProcessBlock( Context, MethodScope, std::move( CaseCopy.ElseBody ), bInLoop,
                                                        std::move( ChildReturnAmbient ), std::move( ChildLoopAmbient ) );
                    Ast.Expr( InnerId ) = Frontend::ExprNode{ std::move( CaseCopy ) };
                    continue;
                }

                {
                    Frontend::BeginExpr BeginCopy = std::get<Frontend::BeginExpr>( Ast.Expr( InnerId ) );

                    std::vector<FinalizeCandidate> ChildReturnAmbient = StmtReturnAmbient;
                    std::vector<FinalizeCandidate> ChildLoopAmbient   = StmtLoopAmbient;

                    BeginCopy.Body = ProcessBlock( Context, MethodScope, std::move( BeginCopy.Body ), bInLoop, ChildReturnAmbient,
                                                   ChildLoopAmbient );
                    for ( const Frontend::StmtId RescueId : BeginCopy.RescueClauses )
                    {
                        Frontend::RescueClause RescueCopy = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
                        // The clause's own bound variable (`rescue e : Exception`) —
                        // an implicit candidate for its own Body only, orthogonal to
                        // whatever this level's own ImplicitCandidate (an outer
                        // enclosing rescue var, if this BeginExpr is itself nested)
                        // already folded into ChildReturnAmbient/ChildLoopAmbient
                        // above.
                        const std::optional<FinalizeCandidate> RescueCandidate =
                            BuildRescueCandidate( Context, RescueCopy, RescueId );
                        RescueCopy.Body      = ProcessBlock( Context, MethodScope, std::move( RescueCopy.Body ), bInLoop,
                                                             ChildReturnAmbient, ChildLoopAmbient, RescueCandidate, RescueCopy.Loc );
                        Ast.Stmt( RescueId ) = Frontend::StmtNode{ std::move( RescueCopy ) };
                    }
                    BeginCopy.EnsureBody = ProcessBlock( Context, MethodScope, std::move( BeginCopy.EnsureBody ), bInLoop,
                                                         std::move( ChildReturnAmbient ), std::move( ChildLoopAmbient ) );
                    Ast.Expr( InnerId )  = Frontend::ExprNode{ std::move( BeginCopy ) };
                }
            }
        }

        // An empty Body (`rescue e : Exception` with nothing in it — a real,
        // legal shape: RaiseUnwind.vl's own fixture) can still carry an
        // ImplicitCandidate: `e` is bound and goes out of scope the instant the
        // clause ends, with no statements to wrap or splice around — just the
        // finalize call itself, at the RescueClause's own Loc (there is no
        // Body[0] to read one from).
        if ( Body.IsEmpty() )
        {
            if ( not ImplicitCandidate.has_value() )
            {
                return Body;
            }
            const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( ImplicitCandidate->Site );
            const Frontend::ExprId CallId =
                BuildFinalizeCall( Context, EmptyBodyLoc, *ImplicitCandidate, LocalType, MethodScope );
            Frontend::StmtList Result;
            Result.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = EmptyBodyLoc, .Expr = CallId } } ) );
            return Result;
        }

        // Body may still need Step 3's splice even with zero LOCAL candidates:
        // an ambient candidate from an enclosing scope must still be finalized
        // at a Return/Break/Next that is a top-level element of THIS Body (a
        // bare `break` inside an `if` with no local of its own, unwinding past
        // the while body's own `x` — see ProcessBlock's own doc comment). Only
        // bail out completely when there is truly nothing to do anywhere.
        if ( Candidates.empty() and ReturnAmbient.empty() and ( not bInLoop or LoopAmbient.empty() ) and
             not ImplicitCandidate.has_value() )
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
                // below applies uniformly to both lists. `return flag ? a : b`
                // needs every branch collected, not just an immediate bare
                // Identifier — see CollectTailIdentifierNames's own doc comment
                // for the use-after-free this recursion exists to prevent.
                std::unordered_set<std::uint32_t> ReturnMovedOutNames;
                if ( bIsReturn )
                {
                    const Frontend::Return &ReturnRef = std::get<Frontend::Return>( Ast.Stmt( StmtId ) );
                    CollectTailIdentifierNames( Ast, ReturnRef.Value, ReturnMovedOutNames );
                }

                std::vector<FinalizeCandidate> ToFinalize     = ReversedBefore( Candidates, BodyPos );
                const std::vector<FinalizeCandidate> &Ambient = bIsReturn ? ReturnAmbient : LoopAmbient;
                ToFinalize.insert( ToFinalize.end(), Ambient.begin(), Ambient.end() );
                // ImplicitCandidate (this Body's own RescueClause-bound
                // variable, if any) is "declared" before every real statement in
                // Body, so it finalizes last — appended after both the local and
                // ambient candidates above, for either kind of spliced exit.
                if ( ImplicitCandidate.has_value() )
                {
                    ToFinalize.push_back( *ImplicitCandidate );
                }

                for ( const FinalizeCandidate &Candidate : ToFinalize )
                {
                    if ( ReturnMovedOutNames.contains( Candidate.Name.Value ) )
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
        if ( Candidates.empty() and not ImplicitCandidate.has_value() )
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
        std::unordered_set<std::uint32_t> MovedOutNames;
        if ( const auto *TailStmt = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Body.Size() - 1] ) ) )
        {
            TailType = Context.Ctx.Values.ExprType( TailStmt->Expr );
            // `flag ? a : b` / `if c then a else b end` used as this Body's own
            // tail must exclude every branch's possible bare-Identifier value,
            // not just an immediate one — see CollectTailIdentifierNames.
            CollectTailIdentifierNames( Ast, TailStmt->Expr, MovedOutNames );
        }

        // Step 5 — reverse declaration order, last-declared first-finalized,
        // covering fall-through / raise / non-local-break through the existing
        // EnsureBody unwind path (BeginRescueEmitter.cpp's EmitBegin).
        Frontend::StmtList EnsureBody;
        for ( auto It = Candidates.rbegin(); It != Candidates.rend(); ++It )
        {
            if ( MovedOutNames.contains( It->Name.Value ) )
            {
                continue;
            }
            const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( It->Site );
            const Frontend::ExprId CallId    = BuildFinalizeCall( Context, Loc, *It, LocalType, MethodScope );
            EnsureBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
        }
        // ImplicitCandidate finalizes last on fall-through too — same "declared
        // before everything else in Body" reasoning as Step 3's splice.
        if ( ImplicitCandidate.has_value() and not MovedOutNames.contains( ImplicitCandidate->Name.Value ) )
        {
            const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( ImplicitCandidate->Site );
            const Frontend::ExprId CallId    = BuildFinalizeCall( Context, Loc, *ImplicitCandidate, LocalType, MethodScope );
            EnsureBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
        }

        // The region's single cleanup boundary. TailType is carried onto it so
        // the region still converges as a value when it stands in expression
        // position (an If/CaseExpr branch feeding an assignment).
        const Frontend::ExprId WrapId =
            Lifetime::EmitBoundary( Ast, Context.Ctx.Values, Loc, std::move( SplicedBody ), std::move( EnsureBody ), TailType );

        Frontend::StmtList Result;
        Result.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = WrapId } } ) );
        return Result;
    }

    // Cheap, approximate pre-check: does Node.Body contain a RescueClause with a
    // bound VarName, anywhere reachable through the same statement-position
    // control-flow constructs ProcessBlock itself recurses into? Doesn't verify
    // the variable's *type* (that needs Values.SiteType, cheap enough on its own
    // but this scan only needs to decide whether ProcessBlock is worth calling
    // at all) — a false positive here just means ProcessBlock runs and finds
    // nothing, never a correctness issue.
    [[nodiscard]] bool ContainsNamedRescueClause ( const Frontend::AstContext &Ast, const Frontend::StmtList &Body )
    {
        for ( const Frontend::StmtId Id : Body )
        {
            if ( not Id.IsValid() )
            {
                continue;
            }
            const Frontend::StmtNode &Node = Ast.Stmt( Id );
            if ( const auto *WhileNode = std::get_if<Frontend::While>( &Node ) )
            {
                if ( ContainsNamedRescueClause( Ast, WhileNode->Body ) )
                {
                    return true;
                }
                continue;
            }
            const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Node );
            if ( ExprStmtNode == nullptr )
            {
                continue;
            }
            const Frontend::ExprNode &Inner = Ast.Expr( ExprStmtNode->Expr );
            if ( const auto *IfNode = std::get_if<Frontend::If>( &Inner ) )
            {
                if ( ContainsNamedRescueClause( Ast, IfNode->Then ) or ContainsNamedRescueClause( Ast, IfNode->Else ) )
                {
                    return true;
                }
            }
            else if ( const auto *CaseNode = std::get_if<Frontend::CaseExpr>( &Inner ) )
            {
                for ( const Frontend::StmtId ClauseId : CaseNode->Clauses )
                {
                    if ( ContainsNamedRescueClause( Ast, std::get<Frontend::WhenClause>( Ast.Stmt( ClauseId ) ).Body ) )
                    {
                        return true;
                    }
                }
                if ( ContainsNamedRescueClause( Ast, CaseNode->ElseBody ) )
                {
                    return true;
                }
            }
            else if ( const auto *BeginNode = std::get_if<Frontend::BeginExpr>( &Inner ) )
            {
                if ( ContainsNamedRescueClause( Ast, BeginNode->Body ) )
                {
                    return true;
                }
                for ( const Frontend::StmtId RescueId : BeginNode->RescueClauses )
                {
                    const auto &Rescue = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
                    if ( Rescue.VarName.IsValid() )
                    {
                        return true;
                    }
                    if ( ContainsNamedRescueClause( Ast, Rescue.Body ) )
                    {
                        return true;
                    }
                }
            }
        }
        return false;
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
            if ( IsFinalizeCandidateType( Context, Context.Ctx.Values.SiteType( BindingIt->second.Site ) ) )
            {
                return true;
            }
        }
        return false;
    }

    // --- Drop on reassign -------------------------------------------------
    //
    // A local that owns a value and is then written again abandons what it
    // held: `result = n.digit_char + result` allocates a new buffer every turn
    // of the loop and strands the previous one. Scope cleanup releases the
    // *last* value only, because that is the only one still named when the
    // scope ends.
    //
    // The release is expressed with nothing new — a sequence in the
    // assignment's own expression slot:
    //
    //     __old = L          # a struct copy: same buffer, second handle
    //     L = <the original assignment, untouched>
    //     __old.finalize()   # releases what L held before the store
    //     L                  # an assignment's value is what was assigned
    //
    // Order is the whole design. The copy is taken *before* the store and
    // released *after* it, so the right-hand side may read `L` freely — which
    // the shape this exists for always does. And the release is a statement of
    // the sequence rather than an `ensure`: on an unwind the store never
    // happened, so `L` still holds the old buffer and scope cleanup will
    // release it; releasing it here as well would be a double free.

    // Every write to `Binding` this body performs, and whether the analysis
    // can see all of them clearly enough to act.
    struct WriteSet
    {
        // Statement-position `Assign` expressions targeting the local, by
        // expression id.
        std::unordered_set<std::uint32_t> Assigns;
        // Every write hands over a value nobody else holds. One that does not
        // — an alias copy, an unprovable call — means the local may not own
        // what it currently holds, and releasing that is a double free.
        bool bAllOwned = true;
        // A write this pass cannot place: nested inside an expression, so
        // there is no statement to sequence a release against.
        bool bOpaqueWrite = false;
    };

    [[nodiscard]] WriteSet CollectWrites ( TypeCheckerContext &Context, const Sema::Binding &Local )
    {
        const Frontend::AstContext &Ast = Context.Ctx.Ast;

        // Which `Assign` expressions stand directly under an `ExprStmt` — the
        // only position a release can be sequenced around.
        std::unordered_set<std::uint32_t> StatementAssigns;
        for ( std::size_t Index = 0; Index < Ast.StmtCount(); ++Index )
        {
            const Frontend::StmtId Id{ static_cast<Frontend::StmtId::ValueType>( Index ) };
            if ( const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Id ) ) )
            {
                if ( ExprStmtNode->Expr.IsValid() )
                {
                    StatementAssigns.insert( ExprStmtNode->Expr.Value );
                }
            }
        }

        WriteSet Out;
        for ( std::size_t Index = 0; Index < Ast.ExprCount(); ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            const auto *AssignNode = std::get_if<Frontend::Assign>( &Ast.Expr( Id ) );
            if ( AssignNode == nullptr or not AssignNode->Target.IsValid() )
            {
                continue;
            }
            if ( not std::holds_alternative<Frontend::Identifier>( Ast.Expr( AssignNode->Target ) ) )
            {
                continue;
            }
            if ( Context.Ctx.Scopes.BindingOf( AssignNode->Target ) != &Local )
            {
                continue;
            }
            Out.bAllOwned = Out.bAllOwned and ProducesOwnedValue( Context, AssignNode->Value );
            if ( StatementAssigns.contains( Id.Value ) )
            {
                Out.Assigns.insert( Id.Value );
            }
            else
            {
                Out.bOpaqueWrite = true;
            }
        }
        return Out;
    }

    // The local's own declaring write, as an expression id — the `Assign`
    // whose `Target` *is* the binding site. A local declared by `x : T = init`
    // binds a `StmtId` instead and has no such expression; it simply never
    // matches, and its later writes then find no declaration to be dominated
    // by, which is the safe answer.
    [[nodiscard]] std::uint32_t DeclaringWriteOf ( const Sema::Binding &Local )
    {
        const auto *Site = std::get_if<Frontend::ExprId>( &Local.Site );
        return Site != nullptr and Site->IsValid() ? Site->Value : 0xFFFFFFFFU;
    }

    // Rewrites one reassignment into the sequence above.
    void EmitDropBeforeStore ( TypeCheckerContext &Context,
                               const Sema::Binding &Local,
                               const Core::Symbol LocalName,
                               const Sema::SemaTypeId LocalType,
                               const Frontend::ExprId AssignId )
    {
        Frontend::AstContext &Ast   = Context.Ctx.Ast;
        const Core::SourceRange Loc = std::get<Frontend::Assign>( Ast.Expr( AssignId ) ).Loc;

        // Parent-less, like every other synthesized region scope: `__old` is
        // never looked up by name, only reached through the Binding captured
        // here (Temporaries' own `RegionScope` comment).
        const Sema::ScopeId RegionScope = Context.Ctx.Scopes.PushScope( Sema::ScopeId{}, Sema::EScopeKind::Branch );
        const Core::Symbol OldName      = Ast.MakeUniqueSymbol( "__old" );

        const auto ReadLocal = [&] ( const bool bRead )
        {
            const Frontend::ExprId Id = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = LocalName } } );
            Context.Ctx.Scopes.BindUse( Id, Local, bRead );
            Context.Ctx.Values.SetExprType( Id, LocalType );
            return Id;
        };

        const Frontend::ExprId OldTarget = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = OldName } } );
        Context.Ctx.Scopes.Declare( RegionScope, OldName, Sema::BindingSite{ OldTarget } );
        const Sema::Binding *OldBound = Context.Ctx.Scopes.Resolve( RegionScope, OldName );
        if ( OldBound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( OldTarget, *OldBound, false );
        }
        Context.Ctx.Values.SetExprType( OldTarget, LocalType );
        Context.Ctx.Values.SetSiteType( Sema::BindingSite{ OldTarget }, LocalType );
        Context.LocalTypes[Sema::BindingSite{ OldTarget }] = LocalType;

        const Frontend::ExprId SaveId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = OldTarget, .Value = ReadLocal( true ) } } );
        Context.Ctx.Values.SetExprType( SaveId, LocalType );

        // The original assignment, moved to a slot of its own so the slot it
        // came from can hold the sequence that now wraps it.
        const Frontend::ExprId StoreId = Ast.Add( Frontend::ExprNode{ Ast.Expr( AssignId ) } );
        Context.Ctx.Values.SetExprType( StoreId, LocalType );

        const Frontend::ExprId ReleaseId = BuildFinalizeCallOnReceiver(
            Context, Loc,
            [&]
            {
                const Frontend::ExprId Id = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = OldName } } );
                if ( OldBound != nullptr )
                {
                    Context.Ctx.Scopes.BindUse( Id, *OldBound, true );
                }
                Context.Ctx.Values.SetExprType( Id, LocalType );
                return Id;
            },
            LocalType );

        Frontend::StmtList RegionBody;
        RegionBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = SaveId } } ) );
        RegionBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = StoreId } } ) );
        RegionBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = ReleaseId } } ) );
        RegionBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = ReadLocal( true ) } } ) );

        EmitSequenceInto( Ast, Context.Ctx.Values, AssignId, Loc, std::move( RegionBody ), LocalType );
    }

    // Walks `Body` in execution order, rewriting every reassignment the
    // declaring write is known to precede.
    //
    // "Known to precede" is deliberately the cheap, sound approximation
    // rules/raii-ownership.md specifies: the declaration must
    // have been passed *in this list or one enclosing it*, never in a sibling
    // branch. A declaration inside an `if` and a reassignment after it is
    // therefore skipped — the local may be uninitialized there, and releasing
    // uninitialized storage is exactly the corruption this model never trades
    // a leak for. Returns whether anything was rewritten.
    bool DropWalkList ( TypeCheckerContext &Context,
                        const Sema::Binding &Local,
                        Core::Symbol LocalName,
                        Sema::SemaTypeId LocalType,
                        const WriteSet &Writes,
                        std::uint32_t DeclaringWrite,
                        const Frontend::StmtList &Body,
                        bool &bDeclared );

    void DropWalkStmt ( TypeCheckerContext &Context,
                        const Sema::Binding &Local,
                        const Core::Symbol LocalName,
                        const Sema::SemaTypeId LocalType,
                        const WriteSet &Writes,
                        const std::uint32_t DeclaringWrite,
                        const Frontend::StmtId Id,
                        bool &bDeclared,
                        bool &bChanged )
    {
        if ( not Id.IsValid() )
        {
            return;
        }

        // Copied out: the rewrite below `Add()`s into both arenas
        // (rules/ast-rewrite.md).
        const Frontend::StmtNode Node = Context.Ctx.Ast.Stmt( Id );

        if ( const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Node ) )
        {
            const Frontend::ExprId Root = ExprStmtNode->Expr;
            if ( Root.IsValid() and Writes.Assigns.contains( Root.Value ) )
            {
                const auto &AssignNode = std::get<Frontend::Assign>( Context.Ctx.Ast.Expr( Root ) );
                if ( AssignNode.Target.Value == DeclaringWrite )
                {
                    bDeclared = true;
                    return;
                }
                if ( bDeclared )
                {
                    EmitDropBeforeStore( Context, Local, LocalName, LocalType, Root );
                    bChanged = true;
                }
                return;
            }
        }

        // Every nested body, at the visibility this statement was reached
        // with. A branch's own `bDeclared` never travels back out: two sibling
        // arms are not on one path.
        std::visit(
            [&] ( const auto &Inner )
            {
                using T = std::remove_cvref_t<decltype( Inner )>;
                if constexpr ( not std::is_same_v<T, std::monostate> )
                {
                    Meta::ForEachField( Inner,
                                        [&] ( const char *, const auto &Field )
                                        {
                                            using F = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                            {
                                                bool bNested = bDeclared;
                                                bChanged     = DropWalkList( Context, Local, LocalName, LocalType, Writes,
                                                                             DeclaringWrite, Field, bNested ) or
                                                           bChanged;
                                            }
                                        } );
                }
            },
            Node );

        // A nested *expression* may hold statements too — an `If`/`CaseExpr`/
        // `BeginExpr` standing in expression position. `ExitPaths` already
        // finds those by where they sit rather than by what encloses them.
        for ( const Frontend::ExprId BlockId : CollectNestedBlockExprs( Context.Ctx.Ast, Id ) )
        {
            const Frontend::ExprNode BlockNode = Context.Ctx.Ast.Expr( BlockId );
            std::visit(
                [&] ( const auto &Inner )
                {
                    using T = std::remove_cvref_t<decltype( Inner )>;
                    if constexpr ( not std::is_same_v<T, std::monostate> )
                    {
                        Meta::ForEachField( Inner,
                                            [&] ( const char *, const auto &Field )
                                            {
                                                using F = std::remove_cvref_t<decltype( Field )>;
                                                if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                                {
                                                    bool bNested = bDeclared;
                                                    bChanged     = DropWalkList( Context, Local, LocalName, LocalType, Writes,
                                                                                 DeclaringWrite, Field, bNested ) or
                                                               bChanged;
                                                }
                                            } );
                    }
                },
                BlockNode );
        }
    }

    bool DropWalkList ( TypeCheckerContext &Context,
                        const Sema::Binding &Local,
                        const Core::Symbol LocalName,
                        const Sema::SemaTypeId LocalType,
                        const WriteSet &Writes,
                        const std::uint32_t DeclaringWrite,
                        const Frontend::StmtList &Body,
                        bool &bDeclared )
    {
        bool bChanged = false;
        // Copied out: `EmitDropBeforeStore` never rewrites a `StmtList`, but it
        // does `Add()` into the Stmt arena, and this list may live in one.
        const Frontend::StmtList Snapshot = Body;
        for ( const Frontend::StmtId Id : Snapshot )
        {
            DropWalkStmt( Context, Local, LocalName, LocalType, Writes, DeclaringWrite, Id, bDeclared, bChanged );
        }
        return bChanged;
    }

} // namespace

bool RunDropOnReassign ( TypeCheckerContext &Context, const Sema::ScopeId Scope, const Frontend::StmtList &Body )
{
    if ( not Scope.IsValid() or Body.IsEmpty() )
    {
        return false;
    }

    bool bChanged               = false;
    const Sema::Scope &ScopeRef = Context.Ctx.Scopes.Get( Scope );
    // Copied out: declaring `__old` below pushes a scope, which may reallocate
    // the scope table and invalidate `ScopeRef` (rules/ast-rewrite.md's
    // discipline, applied to the other arena this pass writes).
    const std::vector<Core::Symbol> Names{ ScopeRef.Order.begin(), ScopeRef.Order.end() };

    for ( const Core::Symbol Name : Names )
    {
        const Sema::Scope &Live = Context.Ctx.Scopes.Get( Scope );
        const auto BindingIt    = Live.Bindings.find( Name );
        if ( BindingIt == Live.Bindings.end() )
        {
            continue;
        }
        const Sema::Binding &Local       = BindingIt->second;
        const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( Local.Site );
        if ( not IsFinalizeCandidateType( Context, LocalType ) )
        {
            continue;
        }

        const std::uint32_t DeclaringWrite = DeclaringWriteOf( Local );
        if ( DeclaringWrite == 0xFFFFFFFFU )
        {
            continue;
        }

        const WriteSet Writes = CollectWrites( Context, Local );
        // Three refusals, each on the leak side of the model's standing
        // arbitration: a write whose value is not provably owned means the
        // local may be holding a borrow; a write this pass cannot place means
        // there are stores it would not sequence a release against; and an
        // escaping local has a second owner that will release the same buffer.
        if ( Writes.Assigns.size() < 2 or not Writes.bAllOwned or Writes.bOpaqueWrite )
        {
            continue;
        }
        if ( EscapesAsCallArgument( Context, Local ) )
        {
            continue;
        }

        bool bDeclared = false;
        bChanged       = DropWalkList( Context, Local, Name, LocalType, Writes, DeclaringWrite, Body, bDeclared ) or bChanged;
    }

    return bChanged;
}

bool RunScopeCleanup ( TypeCheckerContext &Context, const Sema::ScopeId Scope, Frontend::StmtList &Body )
{
    if ( not Scope.IsValid() or Body.IsEmpty() )
    {
        return false;
    }

    // There is no exit-shape bail-out here any more. `ProcessBlock`'s Step 1
    // discovers a nested block through `CollectNestedBlockExprs`, i.e. by
    // where it *is* rather than by which statement shape encloses it, so an
    // exit in expression position (`x = if c then return 1 else 2 end`) is
    // reached by the same recursion as one in statement position and needs
    // no special case to be correct.

    // The silent default this feature must be for ordinary code: a body that
    // owns nothing, and binds no rescue variable, is never touched at all.
    if ( not ScopeHasAnyFinalizeCandidate( Context, Scope ) and not ContainsNamedRescueClause( Context.Ctx.Ast, Body ) )
    {
        return false;
    }

    Body = ProcessBlock( Context, Scope, std::move( Body ), /*bInLoop=*/false, {}, {} );
    return true;
}

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
