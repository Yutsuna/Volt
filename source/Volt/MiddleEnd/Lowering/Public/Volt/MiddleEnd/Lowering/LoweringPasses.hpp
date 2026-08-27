#pragma once

// LoweringPasses.hpp — the Lowering module's whole public surface.
//
// The eight manifest-driven passes (`FunctionalLowering` … `InterpLowering`)
// are *not* declared here: `PassList.inl` names them and `MiddleEnd/Core/
// Pass.hpp` forward-declares them in this namespace, so the registry reaches
// them without any module-to-module include. What this header declares is the
// other half of the module — the sweeps that cannot be manifest passes because
// they need a type that has finished settling, and therefore run from *inside*
// `Analysis::TypeChecker` (and once more per instantiation from
// `TypeSystem::ReinstantiateBody`). See rules/core-ast.md's "the 13 sugar
// nodes": `ArrayLit`/`HashLit`/`Lambda`/`Block` are lowered by these, not by
// an `EPassKind::Lowering` pass.
//
// They all take an `Analysis::TypeCheckerContext &`. That is a back-edge
// against §1's DAG (`Lowering` sits below `Analysis`), and it is deliberate:
// the context is the walk state these sweeps rewrite *within*, so passing it
// is what keeps them sweeps rather than passes. Only the header direction is
// inverted — `TypeCheckerContext.hpp` includes nothing from `Lowering`.

#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "VoltMiddleEndLowering_export.hpp"

namespace Volt::MiddleEnd::Lowering
{

// The vocabulary the sweeps below share with Analysis's own walk. Pre-migration
// every one of these resolved unqualified, because the sweeps and the walk sat
// in the same namespace (`Volt::Sema::TypeCheckerPass`) and the flat
// `Volt::Sema` supplied the type names. Splitting the two apart is what this
// alias block pays for — one place, rather than a qualification at each use.
using Analysis::Resolution;
using Analysis::TypeCheckerContext;
namespace Lifetime = Analysis::Lifetime;

using TypeSystem::Aggregate;
using TypeSystem::EMemberKind;
using TypeSystem::LayoutKind;
using TypeSystem::Member;
using TypeSystem::NominalId;
using TypeSystem::NominalType;
using TypeSystem::SemaType;
using TypeSystem::SemaTypeId;
using TypeSystem::SigTypeId;
using TypeSystem::TypeStore;
using TypeSystem::UnitTypes;

using Resolver::Binding;
using Resolver::BindingSite;
using Resolver::Capture;
using Resolver::ClosureEnvField;
using Resolver::ClosureEnvFrame;
using Resolver::EScopeKind;
using Resolver::Scope;
using Resolver::ScopeId;
using Resolver::ScopeTable;

using Symbol = ::Volt::Core::Symbol;

// ---------------------------------------------------------------------------
// LiteralLowering.cpp — ArrayLit / HashLit / StringLit
// ---------------------------------------------------------------------------

// Rewrites the `ArrayLit` at `Id` in place into `tmp = T.new(); tmp << e0;
// tmp << e1; ...; tmp` (a BeginExpr), using `LiteralType` as its final,
// already-settled SemaTypeId. `Elements` is a caller-owned copy
// (rules/ast-rewrite.md). `<<` is resolved through the ordinary
// operator-resolution path (MemberType, via a synthesized Binary), so a type
// claiming ArrayLit but declaring no `<<` fails with the same diagnostic an
// unresolved method call always gets — see .agents/PLAN_LITERAL_LOWERING.md.
VOLT_MIDDLEEND_LOWERING_EXPORT void
LowerArrayLit ( TypeCheckerContext &Context, Frontend::ExprId Id, Frontend::ExprList Elements, SemaTypeId LiteralType );

// Sweeps the Expr arena, by index, for every `ArrayLit` still standing once
// the whole file's TypeChecker walk has finished — never mid-walk. A literal
// passed as a call argument is InferExpr'd (naturally, bottom-up) *before*
// CheckCallArgs gets to push the parameter's type down via ConstrainExprType
// (CallType: "arguments are bound before being checked"); rewriting inline,
// the moment either path first settles a type, permanently bakes in
// whichever one ran first — wrong the moment a later ConstrainExprType call
// was going to narrow it further. Run only after every constraint in the
// file has had its say, this reads each literal's *final* SemaTypeId
// (Values.ExprType), the same census discipline rules/ast-rewrite.md uses
// for every other arena sweep (see RejectNilableTypes, TypeChecker.cpp).
VOLT_MIDDLEEND_LOWERING_EXPORT void LowerArrayLits ( TypeCheckerContext &Context );

VOLT_MIDDLEEND_LOWERING_EXPORT void LowerHashLit ( TypeCheckerContext &Context,
                                                   Frontend::ExprId Id,
                                                   Frontend::ExprList Keys,
                                                   Frontend::ExprList Values,
                                                   SemaTypeId LiteralType );

VOLT_MIDDLEEND_LOWERING_EXPORT void LowerHashLits ( TypeCheckerContext &Context );

VOLT_MIDDLEEND_LOWERING_EXPORT void
LowerStringLit ( TypeCheckerContext &Context, Frontend::ExprId Id, Frontend::Symbol ValueSym, SemaTypeId LiteralType );

VOLT_MIDDLEEND_LOWERING_EXPORT void LowerStringLits ( TypeCheckerContext &Context );

VOLT_MIDDLEEND_LOWERING_EXPORT void LowerTypeOfExpr ( TypeCheckerContext &Context, Frontend::ExprId Id, SemaTypeId InferredType );

VOLT_MIDDLEEND_LOWERING_EXPORT void LowerTypeOfExprs ( TypeCheckerContext &Context );

// ---------------------------------------------------------------------------
// EnumCaseLowering.cpp — enum constructions and enum patterns
// ---------------------------------------------------------------------------

// Sweeps the Expr arena, by index, for every expression whose
// `CalleeResolution` names an `EnumCase` member — the same "run only after
// every constraint in the file has had its say" discipline `LiteralLowering`
// uses, and for the identical reason (a construction reached as a call
// argument is inferred before the parameter constrains it).
//
// A no-payload case (`Color::Red`, `TaskStatus::InProgress`) rewrites to a
// plain `IntLiteral` carrying the case's ordinal (`Member::EnumOrdinal`) —
// there is nothing to construct, `self` never exists at runtime for these.
// A payload case (`Optional::Some( x )`) rewrites the enclosing `Call` into
// `tmp = T.new(); tmp.tag = ordinal; tmp.<CaseName> = x; tmp` (a
// `BeginExpr`), mirroring `LowerArrayLit`'s shape exactly. Either way the
// backend never sees an `EnumCase` construction — only core-AST it already
// knows how to emit (`rules/backend-machine-only.md`).
VOLT_MIDDLEEND_LOWERING_EXPORT void LowerEnumCases ( TypeCheckerContext &Context );

// Sweeps the Expr arena, by index, for every `CaseExpr` and rewrites each
// `WhenClause` pattern shaped `Call{Callee:Member(target,CaseName),Args:[
// Identifier]}` — CaseLowering's desugar of `.Some(val)` — into a tag
// comparison (`target.tag === ordinal`, the same `===` desugar shape a
// payload-less pattern already gets) plus an implicit binding prepended to
// the clause body: `val = target.<CaseName>`. Every *other* occurrence of
// `val` within the clause's original body (`then val`, `val + 1`, two
// separate uses, ...) is retroactively bound to the same site by a
// recursive `Meta::ForEachField` descent — the same technique
// `ClosureLifting.cpp`'s `RewriteCaptureUses` uses to rewrite every
// occurrence of a captured variable, just matching by *name* against an
// unbound `Identifier` rather than against an existing `ScopeTable`
// binding, since `ScopeResolver` (order 10) ran long before this pattern
// shape existed.
//
// Unlike `LowerEnumCases`, this does **not** skip a deferred (generic-body)
// expression: the case name, its ordinal, and whether it carries a payload
// are per-*nominal* facts (`Member::EnumOrdinal`, `TypeStore::OwnMember`)
// that never depend on a generic argument, so the rewrite can — and must —
// happen once, structurally, on the generic definition's own body; only
// the payload field's *type* is genuinely per-instantiation, and that is
// left to ordinary `Instantiate` substitution exactly as any other member
// access inside a generic body already relies on.
VOLT_MIDDLEEND_LOWERING_EXPORT void LowerEnumPatterns ( TypeCheckerContext &Context );

// ---------------------------------------------------------------------------
// ClosureLifting.cpp — Lambda / Block literals
// ---------------------------------------------------------------------------

// Rewrites the `Lambda`/`Block` at `Id` into `Proc.new( FuncAddr, env )` — a
// `Call`, `env` heap-allocated whenever the literal captures anything — once
// `Id`'s own ClosureType has settled (not deferred, TypeCheckerContext::
// Redirects's own comment: under a generic definition it never settles here
// at all, and this returns without touching Id). The literal's `Params`/
// `Body` are lifted verbatim into a synthesized top-level `Method` Decl
// (`Ast.TopDecls`, the same injection Phase 0's spike proved), recorded in
// `Context.Ctx.Synth` for the backend to declare/define — never in the
// cross-unit TypeStore (`IR::SynthesizedFunctions`'s own doc comment: a
// data race across `Driver::CompileRefs`'s parallel unit sweep otherwise).
//
// `Context.Redirects == nullptr` (an ordinary unit's own TypeChecker pass):
// mutates Id's slot in place, same as any other Lowering rewrite. Non-null
// (TypeSystem::ReinstantiateBody, re-walking a generic body's shared literal
// once per concrete instantiation): every new node still comes from
// `Ast.Add()`, but Id's own slot — and any captured-variable use site inside
// the lifted body — is left untouched and the substitution recorded in
// `*Context.Redirects` instead, so a second instantiation finds the same
// unlowered literal rather than the first instantiation's already-typed answer.
VOLT_MIDDLEEND_LOWERING_EXPORT void LowerClosureLit ( TypeCheckerContext &Context, Frontend::ExprId Id );

// Sweeps the Expr arena, by index, for every `Lambda`/`Block` still standing
// once the whole file's TypeChecker walk has finished — the same "final
// type only" discipline `LowerArrayLits` uses, for the same reason (above).
VOLT_MIDDLEEND_LOWERING_EXPORT void LowerClosureLits ( TypeCheckerContext &Context );

// Reads every closure literal's body and records the two ownership facts a
// callable's single declared member — the `FuncType` claimant's bodyless
// `abstract call` — structurally cannot carry: whether invoking it hands back
// an owned value, and which of its parameters it keeps
// (`TypeCheckerContext::OwnedClosureLiterals` / `ClosureParamEscapes`).
//
// Must run *before* any literal is lifted, because both are keyed by the
// literal's own expression id — which stays the slot the parent calls through
// once the literal has become a `Proc.new`. Called by `LowerClosureLits`, and
// separately by `TypeSystem::ReinstantiateBody`, which lifts literals one at
// a time.
VOLT_MIDDLEEND_LOWERING_EXPORT void AnalyzeClosureLiterals ( TypeCheckerContext &Context );

// ---------------------------------------------------------------------------
// FinalizeLowering.cpp — the RAII finalize sweep
// ---------------------------------------------------------------------------

// Automatic `finalize()` calls — a C++-destructor-flavoured RAII sweep, run
// entirely in the middle-end (rules/backend-machine-only.md: the backend
// gains no new node, only ordinary Call/BeginExpr/Assign it already emits).
//
// A local is a candidate iff it is declared *directly* at the top level of
// SOME StmtList reachable from a `Method::Body` — that Body itself, or the
// body of any nested `If`/`While`/`CaseExpr`-clause/`BeginExpr` it contains,
// at any depth (see CollectCandidates; ProcessBlock in the .cpp recurses
// structurally into each) — its resolved type has `LayoutKind::Aggregate`,
// and that type declares a member named `FinalizeName`. A method with zero
// candidates anywhere is left completely untouched: this must stay true for
// the pass to be a no-op on ordinary code (ScopeHasAnyFinalizeCandidate is
// the cheap up-front check).
//
// ProcessBlock (the .cpp) applies the same wrap-and-splice transform
// independently at every StmtList it visits, innermost first: a nested
// branch's own candidates are finalized on exit from *that* branch, before
// the outer body's own wrap ever runs. Two exit-path mechanisms, both
// zero-backend-change:
//
// - **Fall-through, raise, non-local break** (Phase 1/2): the StmtList is
//   wrapped in a synthetic `BeginExpr{ Body, EnsureBody }`. `EmitBegin`
//   already threads fall-through, unhandled-raise, and non-local-break
//   through `EnsureBody` and re-propagates through enclosing `begin`s —
//   this needs no new backend node at all.
// - **`return`, and loop-owned `break`/`next`** (Phase 3/4): all three
//   bypass `Ensure` entirely today — `EmitReturn` is a raw `CreateRet`, and
//   `EmitBreak`/`EmitNext` inside a loop branch straight to
//   `Frame.Loops.back().{Merge,Latch}` (`StmtReturnBreakNext.cpp`), no
//   ensure-stack lookup in any of the three — so finalize calls are
//   *spliced directly before* each one instead (see ProcessBlock's Step 3).
//   Only an exit that is literally a top-level element of the StmtList being
//   processed is spliced there — but that is no longer a restriction on
//   *which* StmtLists get processed. Since Phase 5, ProcessBlock finds a
//   nested block through `CollectNestedBlockExprs`, i.e. by where the
//   `If`/`CaseExpr`/`BeginExpr` sits rather than by which statement encloses
//   it, so an exit in expression position (`x = if c then return 1 else 2
//   end`, `f( begin ... end )`) reaches the identical recursion a
//   statement-position one does. The old `ContainsUnstructuredExit`
//   bail-out, which refused such a method wholesale, is gone. A spliced exit
//   only finalizes candidates declared strictly before its own position
//   (`BodyIndex < BodyPos`) — anything declared later isn't live on that
//   path yet. `break`/`next` carry no move-out value (the backend refuses
//   `break v`/`next v` inside a loop) and target only the innermost
//   enclosing loop's own body — `bInLoop` is threaded fresh into each nested
//   `While::Body` and inherited unchanged through `If`/`CaseExpr`/
//   `BeginExpr` nesting in between.
//
// Move-out exemption, applied independently at every exit site (a body's own
// fall-through tail, and each spliced top-level `return`): a candidate the
// exit hands back by bare `Identifier` name is skipped at *that* site only —
// freeing it there would hand the caller a dangling buffer. Ownership
// transfers to the caller's own scope, which finalizes it when that scope in
// turn exits. Enumerable#map/#filter/#to_array (source/Lib/Mixins/
// Enumerable.vl) are exactly the implicit-tail-return shape this exemption
// exists for.
//
// Runs from TypeChecker.cpp, right after LowerClosureLits and before the
// Context.Callees snapshot loop — same "final type only, one more post-walk
// sweep inside TypeChecker" shape core-ast.md already documents for
// ArrayLit/HashLit/Lambda/Block. Every synthesized `.finalize()` call is
// resolved through the same InferExpr/MemberType path ordinary source would
// take (rules/backend-machine-only.md's "a synthesized operator is not a
// built-in either"), never a hand-stamped CalleeResolution.
VOLT_MIDDLEEND_LOWERING_EXPORT void InsertFinalizeCalls ( TypeCheckerContext &Context );

} // namespace Volt::MiddleEnd::Lowering
