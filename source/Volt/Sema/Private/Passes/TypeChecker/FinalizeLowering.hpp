#pragma once

#include "TypeCheckerContext.hpp"

namespace Volt::Sema::TypeCheckerPass
{

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
//   processed is spliced there; a `Return`/`Break`/`Next` hiding inside an
//   *expression-position* control construct (`x = if c then return 1 else 2
//   end`) is the one shape ProcessBlock's structural recursion cannot reach
//   — `ContainsUnstructuredExit` detects exactly that and leaves the whole
//   method untouched rather than risk missing a finalize. A spliced exit
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
void InsertFinalizeCalls ( TypeCheckerContext &Context );

} // namespace Volt::Sema::TypeCheckerPass
