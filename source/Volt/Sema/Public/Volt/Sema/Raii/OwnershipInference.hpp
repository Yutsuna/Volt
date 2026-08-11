#pragma once

#include "Sema_export.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <span>

// Does calling this member hand back something the caller owns?
//
// The question RAII cannot avoid and cannot guess. `String#+` returns a
// freshly allocated buffer the caller must release; `Array<T>#[]` returns a
// view onto an element the receiver still owns. Both are "a `Call` whose
// result type declares `finalize`", so classifying a temporary on that shape
// alone is a double free waiting for the second case — which is why
// `Member::bReturnsOwned` exists and why `Owned` must be *proven*, never
// presumed.
//
// It is derived, not annotated. `rules/zero-hardcode.md`'s annotation list
// (`@[Primitive]` `@[External]` `@[Literal]`) is closed, and an
// `@[ReturnsOwned]` would fail that file's own test twice over: it states
// what the compiler should *do* with a result rather than what the result
// *is*, and it is computable from the body the compiler already has. So it is
// computed — once, serially, in the shape the same rule sanctions for
// `bConstructs`/`bIndirect`: take the decision where the information is, and
// record it so no later consumer re-derives it.
namespace Volt::Sema::Raii
{

// Stamps `Member::bReturnsOwned` across the whole store.
//
// **Where this must run**: the serial Driver seam, after
// `ResolveUnitSignatures` (so every member exists and is resolvable) and
// before the parallel `TypeChecker` runs (which read the flag concurrently
// and must never see it move). That is the same seam
// `SynthesizeFinalizeStubs` already uses, one step later.
//
// `Units` is indexed by unit ordinal, exactly like `NominalType::Unit` /
// `Member::Unit`. A null entry is a cache-hit stdlib slot: its members carry
// the flag *from the cache* already (`Member` is a reflected aggregate, so it
// serializes with everything else), and its bodies are deliberately not
// re-analysed.
//
// The analysis is a monotone fixpoint — every flag starts `false` and only
// ever flips to `true` — so it terminates, and an unprovable case (mutual
// recursion, an unresolvable callee) simply stays `false`. `false` means
// `Borrowed`, which costs a leak the metrics count rather than a double free
// no metric can undo.
SEMA_EXPORT void InferReturnOwnership ( std::span<const Frontend::AstContext *const> Units, TypeStore &Store );

} // namespace Volt::Sema::Raii
