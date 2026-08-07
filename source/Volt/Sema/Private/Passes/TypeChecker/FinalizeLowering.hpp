#pragma once

#include "TypeCheckerContext.hpp"

namespace Volt::Sema::TypeCheckerPass
{

// Automatic `finalize()` calls — a C++-destructor-flavoured RAII sweep, run
// entirely in the middle-end (rules/backend-machine-only.md: the backend
// gains no new node, only ordinary Call/BeginExpr/Assign it already emits).
//
// A local is a candidate iff it is declared *directly* at the top level of a
// `Method::Body`'s own flat statement sequence (never nested inside an
// `If`/`While`/`CaseExpr`/`BeginExpr` body — see CollectCandidates), its
// resolved type has `LayoutKind::Aggregate`, and that type declares a member
// named `FinalizeName`. A method with zero candidates is left completely
// untouched: this must stay true for the pass to be a no-op on ordinary code.
//
// Two exit-path mechanisms, both zero-backend-change:
//
// - **Fall-through, raise, non-local break** (Phase 1/2): the method's Body
//   is wrapped in a synthetic `BeginExpr{ Body, EnsureBody }`. `EmitBegin`
//   already threads fall-through, unhandled-raise, and non-local-break
//   through `EnsureBody` and re-propagates through enclosing `begin`s —
//   this needs no new backend node at all.
// - **`return`** (Phase 3): bypasses `Ensure` entirely today — `EmitReturn`
//   (`StmtReturnBreakNext.cpp`) is a raw `CreateRet`, no ensure-stack lookup
//   — so finalize calls are *spliced directly before* each top-level
//   `Return` statement instead (see the Phase-3 block in the .cpp). Only a
//   `Return` that is literally one of `Body`'s own top-level elements is
//   spliced; a `Return` hiding inside a nested branch/loop/begin body
//   (`ContainsNestedReturn`) leaves the whole method untouched (Phase 4).
//   A spliced Return only finalizes candidates declared strictly before its
//   own position (`BodyIndex < BodyPos`) — anything declared later isn't
//   live on that path yet.
//
// Move-out exemption, applied independently at every exit site (the tail
// statement's implicit return, and each spliced top-level `return`): a
// candidate the exit hands back by bare `Identifier` name is skipped at
// *that* site only — freeing it there would hand the caller a dangling
// buffer. Ownership transfers to the caller's own scope, which finalizes it
// when that scope in turn exits. Enumerable#map/#filter/#to_array
// (source/Lib/Mixins/Enumerable.vl) are exactly the implicit-tail-return
// shape this exemption exists for.
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
