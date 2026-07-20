#pragma once

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Volt
{

namespace Sema
{

    // Per-unit counters that passes report back through. This is the one
    // output channel from a pass to the Driver: adding a stat = one field
    // here, incremented by the pass, aggregated by the Driver. Never a
    // reason to widen a pass signature.
    struct PassStats
    {

        std::size_t JsxLowered  = 0;
        std::size_t CaseLowered = 0;
    };

    class InterfaceRegistry;

    // Everything a pass is allowed to touch for one source file. One
    // PassContext per AstContext keeps a whole-circuit compile embarrassingly
    // parallel: passes never reach across files, and diagnostics land in a
    // thread-local Bag that the Driver merges once at the end. Globals is the
    // one read-only exception: the cross-unit interfaces the Driver published
    // serially before the parallel sema phase (null in single-file tools).
    struct PassContext
    {

        Frontend::AstContext &Ast;
        TypeStore &Types;
        Core::DiagEngine::Bag &Diags;
        PassStats &Stats;
        const InterfaceRegistry *Globals = nullptr;
    };

    // A pass is a pure function over a PassContext. New pass = one line in
    // PassList.inl + one definition; the registry and ordering come for free.
    using PassFn = void ( * )( PassContext & );

    // What a pass does to the AST, straight from the manifest's Kind column.
    // The axis tools filter on: `volt parse --lowered` runs only Lowering
    // passes, `check --type` selects Analysis subsets.
    enum class EPassKind : std::uint8_t
    {
        Analysis, // reads the AST, reports diagnostics
        Lowering, // rewrites the AST in place
    };

    struct PassInfo
    {

        std::string_view Name;
        int Order      = 0;
        EPassKind Kind = EPassKind::Analysis;
        PassFn Run     = nullptr;
    };

    // Forward-declare every pass function straight from the manifest.
#define VOLT_PASS( Name, Order, Kind ) void Name( PassContext &Context );
#include "Volt/Sema/PassList.inl"

    // The manifest passes, sorted ascending by Order (built once, cached).
    [[nodiscard]] std::span<const PassInfo> PassRegistry ();

    // Run every registered pass over Context, in Order. Returns the number
    // of passes executed.
    std::size_t RunPasses ( PassContext &Context );

    // Same, restricted to the passes of one Kind (manifest order preserved).
    std::size_t RunPasses ( PassContext &Context, EPassKind Only );

} // namespace Sema

} // namespace Volt
