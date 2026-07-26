#pragma once

#include "Sema_export.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Volt
{

namespace Sema
{

    /// Read one unit's `@[Primitive(...)]` / `@[Literal(...)]` annotations and
    /// bind the types they describe into Store. Returns the number of names
    /// bound.
    ///
    /// This is *not* a pass. A pass is per-file and runs in parallel, but type
    /// binding is inherently cross-unit — `a = 10` in a user file resolves to
    /// the Int32 declared in `source/Lib/Primitives/Int.vl` — so the Driver
    /// calls this serially, in the same seam that publishes interfaces. After
    /// that seam the store is frozen and every sema pass reads it lock-free.
    SEMA_EXPORT std::size_t
    BindUnitTypes ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags );

    /// Second serial phase, over the same units: now that every name exists,
    /// resolve what the declarations *said* — each member's signature, plus
    /// `< Super` and `include`. Split from BindUnitTypes because a signature
    /// routinely names a type declared in a later file, and file order must
    /// never decide what a member returns. Returns the number of signatures
    /// resolved.
    SEMA_EXPORT std::size_t
    ResolveUnitSignatures ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags );

    /// Third serial phase, over every unit at once: attach the structural
    /// `Layout` (the aggregate of a struct/class/mixin/enum with no
    /// `@[Primitive]`) that `BindUnitTypes` deliberately leaves for a
    /// non-primitive type. Split out from both earlier phases because a
    /// field's declared type may itself be an aggregate declared in a
    /// *different* file with no fixed relationship to this one (`Exception`'s
    /// `message : String` is exactly this — nothing before Phase C makes
    /// "String before Exception" true except by accident of file order), so
    /// resolving it needs to jump to that other type's own declaring unit's
    /// AST and recurse — a single linear per-unit pass, the shape Phase A and
    /// B share, cannot do that. `Units` is indexed by the same discovery
    /// ordinal `BindUnitTypes`/`ResolveUnitSignatures` were called with; a
    /// null entry is skipped (a unit with no types needs no slot filled).
    SEMA_EXPORT void ResolveStructLayouts ( std::span<const Frontend::AstContext *const> Units, TypeStore &Store );

} // namespace Sema

} // namespace Volt
