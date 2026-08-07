#pragma once

#include "TypeCheckerContext.hpp"

namespace Volt::Sema::TypeCheckerPass
{

// Automatic `finalize()` calls — a C++-destructor-flavoured RAII sweep, run
// entirely in the middle-end (rules/backend-machine-only.md: the backend
// gains no new node, only ordinary Call/BeginExpr/Assign it already emits).
//
// Phase 1 (this file, so far): a method's fall-through exit only. A local is
// a candidate iff it is declared *directly* at the top level of a
// `Method::Body`'s own flat statement sequence (never nested inside an
// `If`/`While`/`CaseExpr`/`BeginExpr` body — see CollectCandidates), its
// resolved type has `LayoutKind::Aggregate`, and that type declares a member
// named `FinalizeName`. A method containing a `return` anywhere in its body
// is skipped whole — see the `// TODO Phase 3` marker in the .cpp — since
// `return` bypasses `Ensure` entirely today (EmitReturn is a raw `CreateRet`,
// no ensure-stack lookup). A method with zero candidates is left completely
// untouched: this must stay true for the pass to be a no-op on ordinary code.
//
// Move-out exemption: Volt has no explicit `return` in a body this phase
// touches, so the tail statement itself is the return (Ruby-style implicit
// last-expression return). A candidate that the tail hands back by bare name
// (`result` as a body's final statement) is skipped — freeing it here would
// hand the caller a dangling buffer. Enumerable#map/#filter/#to_array
// (source/Lib/Mixins/Enumerable.vl) are exactly this shape and are the
// regression this exemption exists for.
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
