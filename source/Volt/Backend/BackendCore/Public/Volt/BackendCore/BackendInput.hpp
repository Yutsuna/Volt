#pragma once

// BackendInput.hpp — the read-only view of a finished middle-end a backend
// receives. Deliberately *not* Driver types: a backend consumes per-unit
// facts (AST, types, callees, scopes) plus the build-wide TypeStore, never
// the Driver's orchestration state, so the dependency chain stays
// Backend* -> Sema and the Driver maps its CompileUnits into these views.

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/CalleeMap.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"
#include "Volt/Sema/Scope/ScopeTable.hpp"

#include <cstdint>
#include <span>
#include <string_view>

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
        const Frontend::AstContext *Ast  = nullptr;
        const Sema::UnitTypes *Values    = nullptr;
        const Sema::UnitCallees *Callees = nullptr;
        const Sema::ScopeTable *Scopes   = nullptr;
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

        Sema::TypeStore *Types = nullptr;
        std::span<const UnitView> Units;
    };

} // namespace Backend

} // namespace Volt
