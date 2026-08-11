#pragma once

#include "../TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"

// Full-expression regions: the values a *statement* owns without ever naming
// them.
//
// `ScopeCleanup` answers for what a body's locals own. This file answers for
// everything that never became a local at all — the results of `a + b`,
// `arr.map( f )`, `s.trim`, an array literal fed straight to a `for`. Those
// are the majority of the corpus's leaks, and no amount of scope analysis
// finds them, because there is no binding to find.
//
// The model, in one line: **an owned temporary lives until the end of the
// full expression that produced it, unless its ownership is transferred
// first.**
//
//   x = a + b + c
//
//   begin                     # ONE boundary — not one per operator
//     __t0 = a + b            # Owned
//     x    = __t0 + c         # the outer result is Moved into `x`
//   ensure
//     __t0.finalize()         # only what is still owned here
//   end
//
// Three properties this leans on, all of them already true of the middle-end:
//
//   - **`Owned` is proven, never presumed.** A `Call` whose result type
//     declares `finalize` is *not* thereby owned — `Array<T>#[]` hands back a
//     view. Only `Resolution::bConstructs` (a construction) or
//     `Member::bReturnsOwned` (derived by `Raii::InferReturnOwnership` at the
//     Driver seam) qualifies. Everything else is `Borrowed`: a leak the
//     metrics count, never a double free.
//   - **Moves are resolved per outgoing edge**, statically, off the AST's own
//     shape — the value of a `LocalDecl`, of a `Return`, an argument to a
//     constructor. No runtime ownership flag is synthesized
//     (`RaiiRuntimeOwnershipFlags` stays 0), because the edges are visible.
//   - **The rewrite is in place.** A region is introduced by rewriting one
//     expression slot into a boundary; parents refer to the `Id`, so nothing
//     above has to be rebuilt (rules/ast-rewrite.md).
namespace Volt::Sema::TypeCheckerPass::Lifetime
{

// Instruments every full-expression region reachable from `Body`, in place.
//
// Returns true when at least one region was introduced. `Body` itself is
// never reshaped — only the expression slots hanging off it — which is why it
// is taken by const reference and why this composes with `ScopeCleanup`
// running afterwards over the very same list.
bool RunTemporaries ( TypeCheckerContext &Context, const Frontend::StmtList &Body );

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
