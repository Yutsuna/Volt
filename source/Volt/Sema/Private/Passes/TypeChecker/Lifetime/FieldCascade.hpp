#pragma once

#include "../TypeCheckerContext.hpp"

// Field cascade: a type finalizes what it owns, without saying so.
//
// A `struct`/`class` whose fields are themselves finalize-candidates should
// not have to hand-enumerate them in its own `finalize` — the compiler
// appends one `@field.finalize()` per candidate field, in reverse
// field-declaration order, to whatever body the author wrote. That is what
// makes `Hash` need no `finalize` of its own: it allocates nothing, and the
// `Array` behind `@entries` is finalized because it is a *field*, not because
// anyone declared it to be.
//
// This is the *type-shaped* half of RAII: it walks declarations and fields,
// and knows nothing about control flow, regions, or exit paths. The
// value-shaped half — which locals and temporaries a body owns, and where
// they are released — is `ScopeCleanup`/`Temporaries` over in the same
// directory.
namespace Volt::Sema::TypeCheckerPass::Lifetime
{

// Appends the field cascade to every type in this unit that declares its own
// `finalize`.
//
// Runs before the per-`Method` cleanup sweep, so the cascade epilogue is just
// more statements at the natural tail by the time regions are built around
// it — it needs no special handling there.
//
// Two guards, both deliberate:
//
//   - a type with **no** declared `finalize` is skipped here, because
//     synthesizing a whole `Method` this late would never reach TypeBinder's
//     member table. `Raii::FinalizeSynthesis`, at the Driver seam, plants the
//     empty stub for those early enough that this sweep then fills it.
//   - a field the author *already* finalizes by hand is dropped from the
//     cascade, or the buffer would be freed twice (confirmed empirically —
//     see the erase_if below).
void RunFieldCascade ( TypeCheckerContext &Context );

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
