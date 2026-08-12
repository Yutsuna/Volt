#pragma once

#include "../TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"

// Scope regions: the locals a body owns, and where they are released.
//
// This is the value-shaped half of RAII — what `FieldCascade` is to a type,
// this is to a body. It answers three questions for one `StmtList`:
//
//   - which of its locals are `Owned` (declared here, aggregate, declaring
//     `finalize`, and not an alias or an escape);
//   - in what order they are released (LIFO — last declared, first
//     finalized);
//   - which exits carry them, and which hand them to the caller instead.
//
// It recurses into every nested body — `if`/`while`/`when`/`begin` — and
// applies the same transform independently at each, innermost first, so a
// branch's own locals are released on exit from *that* branch. Emission goes
// through `CleanupRegion`; exit discovery through `ExitPaths`; the calls
// themselves through `FinalizeCallBuilder`. This file owns none of those
// three, only the decision of what belongs to whom.
namespace Volt::Sema::TypeCheckerPass::Lifetime
{

// Instruments one body with its scope regions, in place.
//
// Returns false — leaving `Body` untouched — when the body owns nothing worth
// releasing, or when it contains an exit shape this pass refuses to
// half-instrument. Both are silent by design: RAII must be a no-op on
// ordinary code.
[[nodiscard]] bool RunScopeCleanup ( TypeCheckerContext &Context, Sema::ScopeId Scope, Frontend::StmtList &Body );

// Releases what a local held when it is written a second time.
//
// The scope regions above answer "what does this body still own when it
// ends"; this answers the one question they structurally cannot — what it
// owned *in between*. `result = n.digit_char + result` allocates a fresh
// buffer per iteration and strands the previous one, and no exit path ever
// names it. Rewrites the assignment's own expression slot, never a
// `StmtList`, so it composes with everything else here by construction.
[[nodiscard]] bool RunDropOnReassign ( TypeCheckerContext &Context, Sema::ScopeId Scope, const Frontend::StmtList &Body );

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
