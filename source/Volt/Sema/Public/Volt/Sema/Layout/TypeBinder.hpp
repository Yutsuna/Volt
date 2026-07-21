#pragma once

#include "Sema_export.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstddef>
#include <cstdint>

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

} // namespace Sema

} // namespace Volt
