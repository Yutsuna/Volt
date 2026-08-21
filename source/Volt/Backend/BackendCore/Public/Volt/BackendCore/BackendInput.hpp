#pragma once

// BackendInput.hpp — the read-only view of a finished middle-end a backend
// receives. Deliberately *not* Driver types: a backend consumes per-unit
// facts (AST, types, callees, scopes) plus the build-wide TypeStore, never
// the Driver's orchestration state, so the dependency chain stays
// Backend* -> Sema and the Driver maps its CompileUnits into these views.

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/MiddleEnd/IR/CalleeMap.hpp"
#include "Volt/MiddleEnd/IR/SynthesizedFunctions.hpp"
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Volt
{

namespace Backend
{

    // One compile unit, as codegen sees it: everything is settled and
    // read-only. `Module` is the circuit module name (plain text — symbols
    // are per-file, rules/ast-value.md).
    struct UnitView
    {

        // The declaring-unit ordinal the TypeStore keys `Member::Unit` and
        // `NominalType::Unit` on. Not the index of this view: the views are in
        // circuit *link* order while the ordinal is discovery order, and the
        // two differ as soon as a circuit has edges. It is what lets the define
        // sweep read the store — the build-wide resolved interface the declare
        // sweep already uses — and ask "which of these members does *this*
        // unit's AST hold a body for", without a DeclId ever leaving its arena.
        std::uint32_t Ordinal = 0;
        std::string_view Module;
        std::string_view Path;
        const Frontend::AstContext *Ast                = nullptr;
        const MiddleEnd::TypeSystem::UnitTypes *Values = nullptr;
        const MiddleEnd::IR::UnitCallees *Callees      = nullptr;
        const MiddleEnd::Resolver::ScopeTable *Scopes  = nullptr;
        // Functions a lowering pass synthesized for this unit alone
        // (ClosureLifting) — never a TypeStore member, see
        // Sema/Layout/SynthesizedFunctions.hpp. Null in tools that stop
        // before codegen, same contract as the other pointers here.
        const MiddleEnd::IR::SynthesizedFunctions *Synth = nullptr;

        // Unit-scope bindings whose storage belongs to a *different* unit that
        // is already resident, keyed by the binding site this unit knows them
        // by and valued by the exact symbol that storage carries.
        //
        // Null everywhere but a REPL, and a REPL is the only place the
        // situation exists: `x` typed on line 2 is `@_V_global_2_x`, and line
        // 530 has its own binding site for the same storage. Without this the
        // emitter would mint `@_V_global_530_x` — a second, zeroed variable
        // that happens to share a name.
        //
        // A site listed here is *declared*, never defined: the line that owns
        // it defined it, and defining it again is how a session loses every
        // value the user has put in it.
        const std::unordered_map<MiddleEnd::Resolver::BindingSite, std::string, MiddleEnd::Resolver::BindingSiteHash>
            *ExternalGlobals = nullptr;
    };

    // The whole build: units in circuit link order (dependencies before
    // dependents, entry module last), plus the store.
    //
    // `Types` is mutable, and deliberately: everything a backend *reads* is
    // settled, but a generic's layout is not something Sema left out — it is
    // something Sema cannot know. `Array<T>` has no memory shape until T is
    // fixed, and fixing it is monomorphisation, which is codegen
    // (rules/core-ast.md). InstanceLayout.hpp materialises those into the
    // store's layout arena as they are discovered. That arena is separate from
    // the nominal and signature arenas, so growing it cannot invalidate the
    // `Member *` handles Sema published, and codegen is single-threaded.
    //
    // No backend may declare a *type* here — only measure one.
    struct BackendInput
    {

        MiddleEnd::TypeSystem::TypeStore *Types = nullptr;
        std::span<const UnitView> Units;

        // How many of Units' leading entries are the stdlib — ordinals
        // `0..StdlibUnitCount-1` (issue #61's blind-spot-#4 invariant: the
        // stdlib always occupies the low ordinals, cache hit or not). 0 means
        // "nothing here is the stdlib" (a tooling build, or a target that
        // never precompiles it). A backend that never precompiles reads
        // nothing here at all.
        std::uint32_t StdlibUnitCount = 0;
    };

} // namespace Backend

} // namespace Volt
