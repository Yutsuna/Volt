#pragma once

#include "Sema_export.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/CalleeMap.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/SynthesizedFunctions.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"
#include "Volt/Sema/Scope/ScopeTable.hpp"

#include <array>
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

        std::size_t JsxLowered      = 0;
        std::size_t CaseLowered     = 0;
        std::size_t DotCallsLowered = 0;
        std::size_t AssignsLowered  = 0;
        std::size_t IndexesLowered  = 0;
        std::size_t InterpsLowered  = 0;
        // AstInvariant (order 40). Both must be 0 for the AST contract of
        // rules/core-ast.md to hold; both are also reported as hard errors, so
        // these counters are what `check --metrics` shows rather than the
        // enforcement itself.
        std::size_t ResidualSugarNodes = 0;
        std::size_t UntypedValueExprs  = 0;
        std::size_t MacrosExpanded     = 0;
        std::size_t PipelinesLowered   = 0;
        std::size_t MagicsExpanded     = 0;
        std::size_t ScopesResolved     = 0;
        // Not errors: a name ScopeResolver could not bind may be a type or a
        // member — TypeChecker decides later, with type context in hand.
        std::size_t UnresolvedIdentifiers = 0;

        // RAII (InsertFinalizeCalls, the last sweep inside TypeChecker).
        //
        // These exist so that the ownership model is checkable *by the
        // compiler*, rather than only observable through an external
        // valgrind run: an accounting identity holds over them, and a leak
        // shows up as a number here before it shows up as bytes there.
        //
        //     RaiiOwnedCreated == RaiiMoves + RaiiFinalizes + RaiiExplicitEscapes
        //     RaiiOwnedWithoutCleanup == 0
        //     RaiiUnsupportedExits    == 0
        //
        // `RaiiOwnedWithoutCleanup` counts an owned value this pass saw
        // leave its region with neither a move nor a finalize — the
        // instrumentation the plan asks for, deliberately *not* a fix.
        std::size_t RaiiLocals              = 0;
        std::size_t RaiiTemporaries         = 0;
        std::size_t RaiiOwnedCreated        = 0;
        std::size_t RaiiMoves               = 0;
        std::size_t RaiiFinalizes           = 0;
        std::size_t RaiiExplicitEscapes     = 0;
        std::size_t RaiiOwnedWithoutCleanup = 0;
        std::size_t RaiiCleanupPaths        = 0;
        // An exit found inside an *expression-position* control construct —
        // the shape the structural recursion used to refuse wholesale.
        std::size_t RaiiNestedExpressionExits = 0;
        // An exit the pass still cannot instrument. The epic's exit
        // criterion is that this stays 0 on the whole corpus.
        std::size_t RaiiUnsupportedExits = 0;
        // A conditional move resolved with a synthesized runtime flag rather
        // than per-branch on the CFG. The documented fallback: if this ever
        // moves off 0 on ordinary code, the static analysis has regressed.
        std::size_t RaiiRuntimeOwnershipFlags = 0;

        // Fold another unit's counters in, field by field, straight off the
        // reflected shape of this struct. The Driver used to name two of them
        // by hand, so every counter added since was collected per file and
        // then dropped on the floor. Adding a stat stays one field here.
        void Merge ( const PassStats &Other )
        {
            std::array<std::size_t, Meta::FieldCount<PassStats>()> Values{};

            std::size_t Index = 0;
            Meta::ForEachField( Other, [&] ( std::string_view, const std::size_t &Value ) { Values[Index++] = Value; } );
            Index = 0;
            Meta::ForEachField( *this, [&] ( std::string_view, std::size_t &Value ) { Value += Values[Index++]; } );
        }
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
        // Frozen before the parallel sema phase: the Driver binds every
        // unit's types serially first, so passes read it without a lock.
        // const is the enforcement, not a convention.
        const TypeStore &Types;
        // The unit's own inferred expression types. Per-file mutable state,
        // so writing it keeps the parallel phase lock-free.
        UnitTypes &Values;
        // The unit's lexical scopes and name bindings. Written once by
        // ScopeResolver, then a read-only O(1) lookup for later passes.
        ScopeTable &Scopes;
        Core::DiagEngine::Bag &Diags;
        PassStats &Stats;
        const InterfaceRegistry *Globals = nullptr;
        // Registered single-threaded before the parallel phase, then read
        // only — the same contract as Globals. A pass needs it to answer
        // "where am I": path and line/column behind a SourceRange, which no
        // AST node carries on its own. Null in tools that never load files;
        // a pass that depends on it must degrade to a no-op, not crash.
        const Core::SourceManager *Sources = nullptr;
        // The unit's callee resolutions, snapshotted once at the end of
        // TypeChecker so a backend can read them after the pass dies (see
        // Layout/CalleeMap.hpp). Null in tools that stop before codegen —
        // the snapshot is then simply skipped.
        UnitCallees *Callees = nullptr;
        // Functions a lowering pass synthesizes for this unit alone
        // (ClosureLifting) — never registered in the cross-unit TypeStore,
        // for the thread-safety reason SynthesizedFunctions.hpp documents.
        // Per-file mutable state, same contract as Values.
        SynthesizedFunctions &Synth;
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
    [[nodiscard]] SEMA_EXPORT std::span<const PassInfo> PassRegistry ();

    // Run every registered pass over Context, in Order. Returns the number
    // of passes executed.
    SEMA_EXPORT std::size_t RunPasses ( PassContext &Context );

    // Same, restricted to the passes of one Kind (manifest order preserved).
    SEMA_EXPORT std::size_t RunPasses ( PassContext &Context, EPassKind Only );

} // namespace Sema

} // namespace Volt
