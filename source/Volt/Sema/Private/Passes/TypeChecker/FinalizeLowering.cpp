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
#include <optional>
#include <unordered_map>
#include <unordered_set>
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

// Forward declarations — a StmtList's own tail and an expression's possible
// tail values recurse into each other (If/Ternary/CaseExpr/BeginExpr).
void CollectTailIdentifierNames ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out );

// The move-out exemption (design decision 4a/4b) must be conservative about
// *every* path a value can reach the caller through, not just the literal
// syntactic tail — found the hard way: `result = "1" + "2"; flag ? "-" +
// result : result` only checks whether Body's OWN last statement is a bare
// Identifier, and here it's an `If`/`Ternary`, so `result` was never
// recognised as moved out — the method's own EnsureBody then freed the very
// buffer one branch had just handed back by reference, a use-after-free the
// caller's own next read (or double free, if that branch's value is later
// finalized again) can't recover from. This walks every branch of an
// If/Ternary/CaseExpr/BeginExpr tail and collects every name that could be
// the literal value handed back on *some* path — the caller of this
// function excludes all of them, never just the first found.
void CollectTailIdentifierNamesFromBody ( const Frontend::AstContext &Ast,
                                          const Frontend::StmtList &Body,
                                          std::unordered_set<std::uint32_t> &Out )
{
    if ( Body.IsEmpty() )
    {
        return;
    }
    if ( const auto *TailStmt = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Body.Size() - 1] ) ) )
    {
        CollectTailIdentifierNames( Ast, TailStmt->Expr, Out );
    }
}

void CollectTailIdentifierNames ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out )
{
    if ( not Id.IsValid() )
    {
        return;
    }
    if ( const auto *Ident = std::get_if<Frontend::Identifier>( &Ast.Expr( Id ) ) )
    {
        Out.insert( Ident->Name.Value );
        return;
    }
    if ( const auto *Tern = std::get_if<Frontend::Ternary>( &Ast.Expr( Id ) ) )
    {
        CollectTailIdentifierNames( Ast, Tern->Then, Out );
        CollectTailIdentifierNames( Ast, Tern->Else, Out );
        return;
    }
    if ( const auto *IfNode = std::get_if<Frontend::If>( &Ast.Expr( Id ) ) )
    {
        CollectTailIdentifierNamesFromBody( Ast, IfNode->Then, Out );
        CollectTailIdentifierNamesFromBody( Ast, IfNode->Else, Out );
        return;
    }
    if ( const auto *CaseNode = std::get_if<Frontend::CaseExpr>( &Ast.Expr( Id ) ) )
    {
        for ( const Frontend::StmtId ClauseId : CaseNode->Clauses )
        {
            const auto &Clause = std::get<Frontend::WhenClause>( Ast.Stmt( ClauseId ) );
            CollectTailIdentifierNamesFromBody( Ast, Clause.Body, Out );
        }
        CollectTailIdentifierNamesFromBody( Ast, CaseNode->ElseBody, Out );
        return;
    }
    if ( const auto *BeginNode = std::get_if<Frontend::BeginExpr>( &Ast.Expr( Id ) ) )
    {
        CollectTailIdentifierNamesFromBody( Ast, BeginNode->Body, Out );
        for ( const Frontend::StmtId RescueId : BeginNode->RescueClauses )
        {
            const auto &Rescue = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
            CollectTailIdentifierNamesFromBody( Ast, Rescue.Body, Out );
        }
        return;
    }
    // Anything else (Binary, Call, ...) is not a bare passthrough of an
    // existing binding — conservatively still finalized, same as the
    // project's existing "anything more complex than a bare Identifier
    // return is conservative" philosophy.
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
[[nodiscard]] bool IsFinalizeCandidateType ( TypeCheckerContext &Context, Sema::SemaTypeId LocalType )
{
    if ( not LocalType.IsValid() or not Context.Ctx.Values.Has( LocalType ) )
    {
        return false;
    }
    const Sema::NominalId Base = Context.Ctx.Values.Get( LocalType ).Base;
    if ( not Base.IsValid() )
    {
        return false;
    }
    const Sema::NominalType &Nominal = Context.Ctx.Types.Type( Base );
    if ( not Nominal.Layout.IsValid() or Sema::KindOf( Context.Ctx.Types.Get( Nominal.Layout ) ) != Sema::LayoutKind::Aggregate )
    {
        return false;
    }
    return Sema::LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, LocalType, LocalType, FinalizeName ).Decl != nullptr;
}

// Does `Candidate`'s own Binding appear, by bare name, as a positional
// argument to a *constructor* call (`Type.new( x )`, `Resolution::bConstructs`)
// anywhere in this file? Volt has no move/borrow system (the alias-init
// guard just above already documents this same limitation for `a2 = a1`) —
// passing an Aggregate local into `Type.new( x )` is a struct-copy exactly
// like an assignment is, and when the constructor stores the parameter into
// a field via the `@x` shorthand (the overwhelmingly common shape), that
// field now aliases the caller's own local. Two independent finalizes of the
// same buffer is a double free (confirmed empirically: `arr = Array<String>.
// new; arr << "x"; b = Bag.new( arr )` — `arr` finalizes its buffer, then
// `b.finalize`'s field cascade frees the identical pointer again).
//
// Scoped to constructors only, not every call: an ordinary function
// (`drain_event_queue( events )`, ControlFlow/WhileLoop.vl) reads its
// argument transiently and returns, so its own local is correctly still
// live and must still be finalized — the false positive this scoping avoids
// (`WhileLoop.vl` started leaking `pending_events` when this checked every
// call, not just `bConstructs` ones). Within *that* scope this is still
// coarser than checking whether the matched parameter actually carries the
// `@` shorthand: resolving a callee that lives in another unit would need
// that unit's own AstContext, not just the frozen TypeStore's `Member`
// (a signature, not which parameter is instance-var-backed). Treating every
// bare-name constructor argument as a possible escape trades a narrower
// false positive (a constructor that reads its argument transiently, never
// storing it, now leaks that argument instead of double-freeing) for safety
// — leaking is item 1's own already-accepted gap, corruption is not.
// `BindingOf` resolves each argument's own Binding, so a shadowed name in a
// nested scope can never be confused with this candidate's.
[[nodiscard]] bool EscapesAsConstructorArgument ( TypeCheckerContext &Context, const Sema::Binding &CandidateBinding )
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
        if ( ResolutionIt == Context.CalleeResolution.end() or not ResolutionIt->second.bConstructs )
        {
            continue;
        }
        for ( const Frontend::ExprId ArgId : CallNode->Args )
        {
            if ( not ArgId.IsValid() or not std::holds_alternative<Frontend::Identifier>( Ast.Expr( ArgId ) ) )
            {
                continue;
            }
            if ( Context.Ctx.Scopes.BindingOf( ArgId ) == &CandidateBinding )
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
        // initializer is a bare `Identifier`/`InstanceVar` read is, by
        // construction, never a fresh value — it is a struct-copy *alias* of
        // whatever `x`/`@x` already holds (Aggregate locals are copy
        // semantics, not reference semantics — rules/backend-machine-only.md
        // via .agents/backend/abi.md). Finalizing it here as well as the
        // thing it aliases is a double free, confirmed empirically
        // (`a2 = a1; ...` on an `Array<T>` crashes with
        // "double free detected in tcache" without this guard). A
        // Call-initialized local (`x = f(...)`) is not excluded — an
        // ordinary function returning an owned value (the move-out exemption
        // on the *producing* side is what makes that safe) is a distinct,
        // legitimate, already-tested shape (see ReturnMovedOut.vl) — only a
        // *bare* read of an existing binding is refused here.
        if ( InitId.IsValid() and ( std::holds_alternative<Frontend::Identifier>( Ast.Expr( InitId ) ) or
                                    std::holds_alternative<Frontend::InstanceVar>( Ast.Expr( InitId ) ) ) )
        {
            continue;
        }

        const Sema::SemaTypeId LocalType = Context.Ctx.Values.SiteType( Entry.Site );
        if ( not IsFinalizeCandidateType( Context, LocalType ) )
        {
            continue;
        }

        if ( EscapesAsConstructorArgument( Context, Entry ) )
        {
            continue;
        }

        Result.push_back( FinalizeCandidate{ .Site = Entry.Site, .Name = Name, .BodyIndex = BodyIndex } );
    }

    std::ranges::sort( Result,
                       [] ( const FinalizeCandidate &A, const FinalizeCandidate &B ) { return A.BodyIndex < B.BodyIndex; } );
    return Result;
}

[[nodiscard]] Sema::SemaTypeId GetElementFinalizeCandidateType ( TypeCheckerContext &Context, Sema::SemaTypeId LocalType )
{
    if ( not LocalType.IsValid() or not Context.Ctx.Values.Has( LocalType ) )
    {
        return Sema::SemaTypeId{};
    }
    const Sema::SemaType &SemType = Context.Ctx.Values.Get( LocalType );
    if ( SemType.Args.IsEmpty() )
    {
        return Sema::SemaTypeId{};
    }
    const Sema::SemaTypeId ElemType = SemType.Args[0];
    if ( IsFinalizeCandidateType( Context, ElemType ) )
    {
        return ElemType;
    }
    return Sema::SemaTypeId{};
}

// `<receiver>.finalize()`, cascading into `<receiver>[i].finalize()` first
// when the receiver's own type carries a finalize-candidate element type
// (`GetElementFinalizeCandidateType` — Array<T>'s own T, resolved the same
// structural way any other candidacy is: no bound, no Volt name spelled
// here, just "does the element type declare finalize"). `MakeReadId` is
// called once per fresh occurrence of the receiver the synthesized tree
// needs (size read, each index read, the final finalize call) — value-AST
// nodes are single-owner, so a receiver used N times needs N distinct nodes
// (rules/ast-value.md). Shared by both a local/rescue candidate
// (`BuildFinalizeCall`, an Identifier read through scope Resolve/BindUse)
// and a field (`BuildFieldFinalizeCall`, an InstanceVar read) — the two
// differ only in what kind of node MakeReadId builds.
template <typename MakeReadIdFn>
[[nodiscard]] Frontend::ExprId BuildFinalizeCallOnReceiver ( TypeCheckerContext &Context,
                                                             Core::SourceRange Loc,
                                                             MakeReadIdFn MakeReadId,
                                                             Sema::SemaTypeId ValueType )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const Sema::SemaTypeId ElemType = GetElementFinalizeCandidateType( Context, ValueType );
    if ( ElemType.IsValid() )
    {
        // A fresh Scope per synthesized loop (Parent invalid — `__fin_i` is
        // never looked up by name, only ever read back through the Binding
        // captured below, so it needs no lexical parent chain). Every
        // occurrence of `__fin_i` must BindUse the *same* Binding — that is
        // what lets codegen's SlotFor (EmitAddress, ExprPlaceEmitter.cpp)
        // find one persistent stack slot for all of them; an Identifier
        // reaching codegen with no Binding at all is refused loudly there
        // ("reached codegen with no scope binding"), which is exactly what
        // silently never fired here before: this whole branch used to be
        // gated on `Types.LookupNodeKind( "UInt64" )`, a category error
        // (LookupNodeKind answers "which type claims this *AST node kind*",
        // e.g. `IntLiteral`/`PointerType` — never "which type is named
        // this"; no type claims a node kind literally spelled `UInt64`, so
        // the lookup always failed and this entire cascade was dead code).
        const Sema::ScopeId LoopScope   = Context.Ctx.Scopes.PushScope( Sema::ScopeId{}, Sema::EScopeKind::Branch );
        const Core::Symbol IdxName      = Ast.MakeUniqueSymbol( "__fin_i" );
        const Sema::Binding *IdxBinding = nullptr;

        auto MakeIdxReadId = [&] () -> Frontend::ExprId
        {
            const Frontend::ExprId Id = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = IdxName } } );
            if ( IdxBinding != nullptr )
            {
                Context.Ctx.Scopes.BindUse( Id, *IdxBinding, true );
            }
            return Id;
        };

        // 1. Init: __fin_i = 0 — the literal carries no suffix and gets no
        // manual type override: it types the same way any other synthesized
        // IntLiteral in this codebase does (ClosureLifting's RunningOffset/
        // SizeOfType — whatever type claims the `IntLiteral` node kind,
        // i.e. Int32), and `candidate.size`/`[]` are UInt64-shaped, so the
        // comparison and index both go through the ordinary same-family
        // widening rule (`rules/zero-hardcode.md`'s "Implicit widening
        // between scalars of the same family") rather than a hand-picked
        // UInt64 nominal.
        const Frontend::ExprId ZeroLitId =
            Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( "0" ) } } );
        InferExpr( Context, ZeroLitId );

        const Frontend::ExprId IdxTargetId = MakeIdxReadId(); // declaring occurrence — IdxBinding is still null here
        Context.Ctx.Scopes.Declare( LoopScope, IdxName, Sema::BindingSite{ IdxTargetId } );
        IdxBinding = Context.Ctx.Scopes.Resolve( LoopScope, IdxName );
        if ( IdxBinding != nullptr )
        {
            Context.Ctx.Scopes.BindUse( IdxTargetId, *IdxBinding, false ); // a write, not a use — ScopeTable.hpp's own convention
        }

        const Frontend::ExprId InitAssignId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = IdxTargetId, .Value = ZeroLitId } } );
        InferExpr( Context, InitAssignId );
        const Frontend::StmtId InitStmtId =
            Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = InitAssignId } } );

        // 2. Condition: __fin_i < candidate.size
        const Frontend::ExprId ReadIdSize = Ast.Add( Frontend::ExprNode{
            Frontend::Member{ .Loc = Loc, .Object = MakeReadId(), .Name = Ast.Strings().Intern( "size" ) } } );
        InferExpr( Context, ReadIdSize );

        const Frontend::ExprId CondId = Ast.Add( Frontend::ExprNode{
            Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Lt, .Lhs = MakeIdxReadId(), .Rhs = ReadIdSize } } );
        InferExpr( Context, CondId );

        // 3. Body statement 1: candidate[__fin_i].finalize() — built directly
        // in `Index`'s own already-desugared form (`Call( Member( x, "[]" ),
        // [ i ] )`, IndexLowering.cpp): `Index` is `VOLT_EXPR_SUGAR`
        // (rules/core-ast.md), a `Lowering`-kind pass rewrites it before
        // TypeChecker runs, and this synthesis happens *inside* TypeChecker
        // — a raw `Index` node here would survive to `AstInvariant` and be
        // refused as residual sugar.
        const Frontend::ExprId IndexArgsId   = MakeIdxReadId();
        const Frontend::ExprId IndexCalleeId = Ast.Add(
            Frontend::ExprNode{ Frontend::Member{ .Loc = Loc, .Object = MakeReadId(), .Name = Ast.Strings().Intern( "[]" ) } } );
        const Frontend::ExprId ElementId = Ast.Add( Frontend::ExprNode{
            Frontend::Call{ .Loc = Loc, .Callee = IndexCalleeId, .Args = { IndexArgsId }, .ArgNames = {}, .BlockArg = {} } } );
        InferExpr( Context, ElementId );

        const Frontend::ExprId ElemMemberId = Ast.Add( Frontend::ExprNode{
            Frontend::Member{ .Loc = Loc, .Object = ElementId, .Name = Ast.Strings().Intern( FinalizeName ) } } );
        const Frontend::ExprId ElemCallId   = Ast.Add( Frontend::ExprNode{
            Frontend::Call{ .Loc = Loc, .Callee = ElemMemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );
        InferExpr( Context, ElemCallId );
        const Frontend::StmtId ElemStmtId = Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = ElemCallId } } );

        // 4. Body statement 2: __fin_i = __fin_i + 1
        const Frontend::ExprId OneLitId =
            Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( "1" ) } } );
        InferExpr( Context, OneLitId );

        const Frontend::ExprId AddId = Ast.Add( Frontend::ExprNode{
            Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Plus, .Lhs = MakeIdxReadId(), .Rhs = OneLitId } } );
        InferExpr( Context, AddId );

        const Frontend::ExprId IncrAssignId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = MakeIdxReadId(), .Value = AddId } } );
        InferExpr( Context, IncrAssignId );
        const Frontend::StmtId IncrStmtId =
            Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = IncrAssignId } } );

        // 5. While loop: while __fin_i < candidate.size ... end
        Frontend::StmtList LoopBody;
        LoopBody.PushBack( ElemStmtId );
        LoopBody.PushBack( IncrStmtId );

        const Frontend::StmtId WhileStmtId =
            Ast.Add( Frontend::StmtNode{ Frontend::While{ .Loc = Loc, .Cond = CondId, .Body = std::move( LoopBody ) } } );

        // 6. Finalize candidate call: candidate.finalize()
        const Frontend::ExprId MemberId = Ast.Add( Frontend::ExprNode{
            Frontend::Member{ .Loc = Loc, .Object = MakeReadId(), .Name = Ast.Strings().Intern( FinalizeName ) } } );
        const Frontend::ExprId CallId   = Ast.Add(
            Frontend::ExprNode{ Frontend::Call{ .Loc = Loc, .Callee = MemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );
        InferExpr( Context, CallId );
        const Frontend::StmtId FinalizeStmtId = Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } );

        // 7. BeginExpr wrapping the sequence
        Frontend::StmtList BeginBody;
        BeginBody.PushBack( InitStmtId );
        BeginBody.PushBack( WhileStmtId );
        BeginBody.PushBack( FinalizeStmtId );

        const Frontend::ExprId BeginExprId = Ast.Add( Frontend::ExprNode{
            Frontend::BeginExpr{ .Loc = Loc, .Body = std::move( BeginBody ), .RescueClauses = {}, .EnsureBody = {} } } );
        Context.Ctx.Values.SetExprType( BeginExprId, Context.Ctx.Values.ExprType( CallId ) );
        return BeginExprId;
    }

    const Frontend::ExprId ReadId   = MakeReadId();
    const Frontend::ExprId MemberId = Ast.Add(
        Frontend::ExprNode{ Frontend::Member{ .Loc = Loc, .Object = ReadId, .Name = Ast.Strings().Intern( FinalizeName ) } } );
    const Frontend::ExprId CallId = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = Loc, .Callee = MemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );
    InferExpr( Context, CallId );
    return CallId;
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
            if ( ImplicitCandidate.has_value() )
            {
                ChildReturnAmbient.push_back( *ImplicitCandidate );
            }

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
            if ( ImplicitCandidate.has_value() )
            {
                ChildReturnAmbient.push_back( *ImplicitCandidate );
                if ( bInLoop )
                {
                    ChildLoopAmbient.push_back( *ImplicitCandidate );
                }
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
            if ( ImplicitCandidate.has_value() )
            {
                ChildReturnAmbient.push_back( *ImplicitCandidate );
                if ( bInLoop )
                {
                    ChildLoopAmbient.push_back( *ImplicitCandidate );
                }
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
            if ( ImplicitCandidate.has_value() )
            {
                ChildReturnAmbient.push_back( *ImplicitCandidate );
                if ( bInLoop )
                {
                    ChildLoopAmbient.push_back( *ImplicitCandidate );
                }
            }

            BeginCopy.Body =
                ProcessBlock( Context, MethodScope, std::move( BeginCopy.Body ), bInLoop, ChildReturnAmbient, ChildLoopAmbient );
            for ( const Frontend::StmtId RescueId : BeginCopy.RescueClauses )
            {
                Frontend::RescueClause RescueCopy = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
                // The clause's own bound variable (`rescue e : Exception`) —
                // an implicit candidate for its own Body only, orthogonal to
                // whatever this level's own ImplicitCandidate (an outer
                // enclosing rescue var, if this BeginExpr is itself nested)
                // already folded into ChildReturnAmbient/ChildLoopAmbient
                // above.
                const std::optional<FinalizeCandidate> RescueCandidate = BuildRescueCandidate( Context, RescueCopy, RescueId );
                RescueCopy.Body = ProcessBlock( Context, MethodScope, std::move( RescueCopy.Body ), bInLoop, ChildReturnAmbient,
                                                ChildLoopAmbient, RescueCandidate, RescueCopy.Loc );
                Ast.Stmt( RescueId ) = Frontend::StmtNode{ std::move( RescueCopy ) };
            }
            BeginCopy.EnsureBody = ProcessBlock( Context, MethodScope, std::move( BeginCopy.EnsureBody ), bInLoop,
                                                 std::move( ChildReturnAmbient ), std::move( ChildLoopAmbient ) );
            Ast.Expr( InnerId )  = Frontend::ExprNode{ std::move( BeginCopy ) };
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
        const Frontend::ExprId CallId    = BuildFinalizeCall( Context, EmptyBodyLoc, *ImplicitCandidate, LocalType, MethodScope );
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

// A struct/class's own `finalize` should not need to hand-enumerate every
// Aggregate-typed field itself (Exception#finalize did, by hand, before
// this). Scoped to a type that already declares its own `finalize` —
// synthesizing a brand-new Method Decl for a type with *no* declared
// `finalize` would need registering with TypeStore/ScopeResolver *before*
// TypeChecker runs (the same reason ClosureLifting's synthesized functions
// get their own dedicated SynthesizedFunctions table rather than an
// ordinary Decl append at this pass's own late stage — a Decl added here
// would never be visible to TypeBinder's member table or the backend's own
// per-type emission walk). CASCADE_FINALIZE.md tracks that half separately.
// Also scoped to non-generic types only: a generic type's field types are
// not resolved until instantiation (the same MonoDriver-lives-only-in-
// BackendLLVM wall CASCADE_FINALIZE.md's item 2 hits).

// Forward declarations — a StmtId's own fields and an ExprId's own fields
// recurse into each other, same shape as ContainsExitStmt/ContainsExitExpr
// above.
void CollectHandFinalizedFieldsStmt ( const Frontend::AstContext &Ast,
                                      Frontend::StmtId Id,
                                      std::unordered_set<std::uint32_t> &Out );
void CollectHandFinalizedFields ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out );

// Same reflective-descent shape as ScanExitFields above (Meta::ForEachField,
// not a hand-picked switch over every node kind — rules/meta-first.md), but
// collecting a set instead of a bool: every `@field.finalize` (or
// `@field.finalize()`) call reachable anywhere in a user-written `finalize`
// body, keyed by the InstanceVar's own interned spelling (kept with its
// leading `@`, matching `BuildFieldFinalizeCall`'s own `AtName`).
template <typename NodeVariant>
void ScanHandFinalizedFields ( const Frontend::AstContext &Ast,
                               const NodeVariant &Variant,
                               std::unordered_set<std::uint32_t> &Out )
{
    std::visit(
        [&] ( const auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Meta::ForEachField( Node,
                                    [&] ( const char *, const auto &Field )
                                    {
                                        using F = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                        {
                                            CollectHandFinalizedFields( Ast, Field, Out );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                CollectHandFinalizedFields( Ast, Child, Out );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            CollectHandFinalizedFieldsStmt( Ast, Field, Out );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                CollectHandFinalizedFieldsStmt( Ast, Child, Out );
                                            }
                                        }
                                    } );
            }
        },
        Variant );
}

void CollectHandFinalizedFieldsStmt ( const Frontend::AstContext &Ast,
                                      Frontend::StmtId Id,
                                      std::unordered_set<std::uint32_t> &Out )
{
    if ( not Id.IsValid() )
    {
        return;
    }
    ScanHandFinalizedFields( Ast, Ast.Stmt( Id ), Out );
}

void CollectHandFinalizedFields ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out )
{
    if ( not Id.IsValid() )
    {
        return;
    }
    const Frontend::ExprNode &Node = Ast.Expr( Id );
    if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
    {
        if ( CallNode->Callee.IsValid() )
        {
            if ( const auto *MemberNode = std::get_if<Frontend::Member>( &Ast.Expr( CallNode->Callee ) ) )
            {
                if ( MemberNode->Object.IsValid() and Ast.Text( MemberNode->Name ) == FinalizeName )
                {
                    if ( const auto *IVarNode = std::get_if<Frontend::InstanceVar>( &Ast.Expr( MemberNode->Object ) ) )
                    {
                        Out.insert( IVarNode->Name.Value );
                    }
                }
            }
        }
    }
    ScanHandFinalizedFields( Ast, Node, Out );
}

// Every Field declared directly in Body whose resolved type is an
// Aggregate-layout, finalize-declaring candidate — resolved straight off
// each Field's own written TypeRef (ResolveTypeExpr), never through
// TypeStore/NominalId machinery: a non-generic type's field types need no
// instantiation, so there is nothing generic-argument-shaped to substitute.
[[nodiscard]] std::vector<std::pair<Core::Symbol, Sema::SemaTypeId>> CollectCascadeFields ( TypeCheckerContext &Context,
                                                                                            const Frontend::DeclList &Body )
{
    std::vector<std::pair<Core::Symbol, Sema::SemaTypeId>> Result;
    const Frontend::AstContext &Ast = Context.Ctx.Ast;
    for ( const Frontend::DeclId Id : Body )
    {
        if ( not Id.IsValid() )
        {
            continue;
        }
        const auto *FieldNode = std::get_if<Frontend::Field>( &Ast.Decl( Id ) );
        if ( FieldNode == nullptr )
        {
            continue;
        }
        Sema::UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Sema::SemaTypeId{}, .Bindings = {} };
        const Sema::SemaTypeId FieldType =
            Sema::ResolveTypeExpr( Ast, Context.Ctx.Types, /*Generics=*/{}, Sink, FieldNode->DeclType );
        if ( not IsFinalizeCandidateType( Context, FieldType ) )
        {
            continue;
        }
        Result.emplace_back( FieldNode->Name, FieldType );
    }
    return Result;
}

// The Struct/Class's own `finalize`, if directly declared in Body — a plain
// AST scan, not a TypeStore lookup: answering "is one of Body's own
// elements a Method named finalize" needs no member-resolution machinery.
[[nodiscard]] std::optional<Frontend::DeclId> FindOwnFinalizeMethod ( const Frontend::AstContext &Ast,
                                                                      const Frontend::DeclList &Body )
{
    for ( const Frontend::DeclId Id : Body )
    {
        if ( not Id.IsValid() )
        {
            continue;
        }
        if ( const auto *MethodNode = std::get_if<Frontend::Method>( &Ast.Decl( Id ) ) )
        {
            if ( Ast.Text( MethodNode->Name ) == FinalizeName )
            {
                return Id;
            }
        }
    }
    return std::nullopt;
}

// `@field.finalize()` — mirrors BuildFinalizeCall's own shape (manual type
// on the receiver, InferExpr for the Member/Call, and the same
// BuildFinalizeCallOnReceiver element cascade when @field's own type is
// itself Array-shaped over a finalize-candidate element — .agents/
// CASCADE_FINALIZE.md item 2 needed no bound at all: the cascade is driven
// structurally, off the field's element type declaring `finalize`, exactly
// like every other candidacy check in this file), but the receiver is an
// InstanceVar rather than an Identifier: field access needs no scope
// Binding at all (ExprPlaceEmitter reads it off Frame().Self directly at
// codegen), and resolving it through the ordinary InstanceVar arm of
// InferExpr would need Context.SelfValue set to this type — not ambient
// here, since this step runs over every Struct/Class Decl in the file,
// outside any per-method walk. Setting the type directly (InferExpr's own
// cache check memoizes it, ExprInferencer.cpp) sidesteps that without
// needing to fake a Self context.
[[nodiscard]] Frontend::ExprId
BuildFieldFinalizeCall ( TypeCheckerContext &Context, Core::SourceRange Loc, Core::Symbol FieldName, Sema::SemaTypeId FieldType )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // The written spelling keeps its `@` (ExprPlaceEmitter's own doc
    // comment on InstanceVar) — Field.Name never carries one, so this
    // synthesizes the same spelling a hand-written `@message` token would
    // have interned.
    const Core::Symbol AtName = Ast.Strings().Intern( "@" + std::string( Ast.Text( FieldName ) ) );

    auto MakeReadId = [&] () -> Frontend::ExprId
    {
        const Frontend::ExprId IVarId = Ast.Add( Frontend::ExprNode{ Frontend::InstanceVar{ .Loc = Loc, .Name = AtName } } );
        Context.Ctx.Values.SetExprType( IVarId, FieldType );
        return IVarId;
    };

    return BuildFinalizeCallOnReceiver( Context, Loc, MakeReadId, FieldType );
}

// Appends one `@field.finalize()` ExprStmt per cascade-candidate field to
// the end of an *existing*, user-declared `finalize` method's own Body.
// Reverse field declaration order (last-declared, first-finalized),
// matching every other finalize ordering this pass already produces. Runs
// before InsertFinalizeCalls' own per-Method loop, so the ordinary
// candidate/wrap machinery (ProcessBlock) sees the cascade calls as part of
// the body it instruments, exactly like a hand-written tail.
void AppendFieldCascade ( TypeCheckerContext &Context,
                          Frontend::DeclId FinalizeMethodId,
                          const std::vector<std::pair<Core::Symbol, Sema::SemaTypeId>> &Fields )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Copied out before any Add() below appends to the Expr/Stmt arenas
    // (rules/ast-rewrite.md).
    Frontend::Method Node = std::get<Frontend::Method>( Ast.Decl( FinalizeMethodId ) );

    // A user-declared `finalize` with a genuinely empty body (only the
    // cascade needed, nothing hand-written) is a legitimate shape — there is
    // no Body[0] to read a Loc off, so fall back to the Method's own.
    Core::SourceRange Loc = Node.Loc;
    if ( not Node.Body.IsEmpty() )
    {
        std::visit(
            [&] ( const auto &N )
            {
                using T = std::remove_cvref_t<decltype( N )>;
                if constexpr ( not std::is_same_v<T, std::monostate> )
                {
                    Loc = N.Loc;
                }
            },
            Ast.Stmt( Node.Body[Node.Body.Size() - 1] ) );
    }

    for ( auto It = Fields.rbegin(); It != Fields.rend(); ++It )
    {
        const Frontend::ExprId CallId = BuildFieldFinalizeCall( Context, Loc, It->first, It->second );
        Node.Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
    }

    Ast.Decl( FinalizeMethodId ) = Frontend::DeclNode{ std::move( Node ) };
}

} // namespace

void Volt::Sema::TypeCheckerPass::InsertFinalizeCalls ( TypeCheckerContext &Context )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Field cascade (.agents/CASCADE_FINALIZE.md item 3) — runs first, over
    // every Struct/Class Decl, so a type's own `finalize` (if it has one)
    // already carries its field-cascade epilogue by the time the ordinary
    // per-Method loop below instruments it. Never adds a Decl (only
    // appends Stmts to an *existing* Method's Body), so it needs no
    // DeclCount snapshot of its own — the arena's Decl slots are stable
    // across this loop.
    for ( std::size_t Index = 0; Index < Ast.DeclCount(); ++Index )
    {
        const Frontend::DeclId Id{ static_cast<Frontend::DeclId::ValueType>( Index ) };
        const Frontend::DeclList *Body = nullptr;
        if ( const auto *StructNode = std::get_if<Frontend::Struct>( &Ast.Decl( Id ) ) )
        {
            if ( StructNode->Generics.IsEmpty() )
            {
                Body = &StructNode->Body;
            }
        }
        else if ( const auto *ClassNode = std::get_if<Frontend::Class>( &Ast.Decl( Id ) ) )
        {
            if ( ClassNode->Generics.IsEmpty() )
            {
                Body = &ClassNode->Body;
            }
        }
        if ( Body == nullptr )
        {
            continue;
        }

        const std::optional<Frontend::DeclId> FinalizeId = FindOwnFinalizeMethod( Ast, *Body );
        if ( not FinalizeId.has_value() )
        {
            continue;
        }
        std::vector<std::pair<Core::Symbol, Sema::SemaTypeId>> Fields = CollectCascadeFields( Context, *Body );
        if ( Fields.empty() )
        {
            continue;
        }

        // A user-written `finalize` may already call `@field.finalize`
        // itself (the only correct way to write one before this cascade
        // existed — Exception.vl's own former backtrace loop did exactly
        // this). Appending the cascade on top unconditionally would double
        // free that field (confirmed empirically: a `finalize` whose whole
        // body is `@s.finalize` had its single field freed twice once the
        // cascade landed). Only cascade fields the body does not already
        // finalize by hand.
        const auto &FinalizeMethod = std::get<Frontend::Method>( Ast.Decl( *FinalizeId ) );
        std::unordered_set<std::uint32_t> HandFinalized;
        for ( const Frontend::StmtId StmtId : FinalizeMethod.Body )
        {
            CollectHandFinalizedFieldsStmt( Ast, StmtId, HandFinalized );
        }
        std::erase_if( Fields,
                       [&] ( const std::pair<Core::Symbol, Sema::SemaTypeId> &Entry )
                       {
                           const Core::Symbol AtName = Ast.Strings().Intern( "@" + std::string( Ast.Text( Entry.first ) ) );
                           return HandFinalized.contains( AtName.Value );
                       } );
        if ( Fields.empty() )
        {
            continue;
        }

        AppendFieldCascade( Context, *FinalizeId, Fields );
    }

    // A file's own top-level statements are a distinct list from any Method
    // body (Ast.TopStmts, a plain std::vector<StmtId> — never a
    // Frontend::StmtList — walked under the Unit root Scope ScopeResolver
    // pushes once per file, ScopeResolver.cpp's own `WalkStmt( Id, Root )`
    // loop). The Decl-only loop below would silently skip every top-level
    // `begin/rescue`, `Array<T>.new(...)` local, etc. — exactly the shape
    // BeginRescue.vl's and RaiseUnwind.vl's own top-level `begin ... rescue
    // e ... end` use, previously never finalized at all. Converted to a
    // StmtList and back — ProcessBlock's contract, not TopStmts' own.
    if ( not Ast.TopStmts.empty() )
    {
        Frontend::StmtList TopBody;
        for ( const Frontend::StmtId Id : Ast.TopStmts )
        {
            TopBody.PushBack( Id );
        }

        const Sema::ScopeId UnitScope = Context.Ctx.Scopes.ScopeOf( Ast.TopStmts[0] );
        if ( UnitScope.IsValid() and not ContainsUnstructuredExit( Ast, TopBody ) and
             ( ScopeHasAnyFinalizeCandidate( Context, UnitScope ) or ContainsNamedRescueClause( Ast, TopBody ) ) )
        {
            TopBody = ProcessBlock( Context, UnitScope, std::move( TopBody ), /*bInLoop=*/false, {}, {} );
            Ast.TopStmts.clear();
            for ( const Frontend::StmtId Id : TopBody )
            {
                Ast.TopStmts.push_back( Id );
            }
        }
    }

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

        if ( not ScopeHasAnyFinalizeCandidate( Context, MethodScope ) and not ContainsNamedRescueClause( Ast, Node.Body ) )
        {
            continue;
        }

        Node.Body      = ProcessBlock( Context, MethodScope, std::move( Node.Body ), /*bInLoop=*/false, {}, {} );
        Ast.Decl( Id ) = Frontend::DeclNode{ std::move( Node ) };
    }
}
