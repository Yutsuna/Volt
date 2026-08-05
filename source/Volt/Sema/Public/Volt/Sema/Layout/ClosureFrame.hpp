#pragma once

#include "Sema_export.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Scope/ScopeTable.hpp"

namespace Volt
{

namespace Sema
{

    // Structural only — which variables are captured, in what order, and
    // their Site/Type/Name. Byte sizing (offset within the env buffer, total
    // allocation size) cannot be known here: under a generic definition a
    // captured field's type is unresolved, and even once concrete, byte size
    // is LayoutEngine's answer, resolved by the backend right before it is
    // needed (rules/backend-machine-only.md). ClosureLifting.cpp builds
    // `SizeOf`-based offset/size expressions from this list instead of a
    // baked-in constant.
    struct ClosureEnvField
    {
        Symbol Name;
        BindingSite Site;
        SemaTypeId Type;
    };

    struct ClosureEnvFrame
    {
        ScopeId Scope;
        Core::SmallVec<ClosureEnvField, 4> Fields;

        // false only for a closure literal consumed directly at its call
        // site (ScopeTable::Escapes) — codegen may stack-allocate that
        // environment. Any other closure, including one with zero captures
        // that is still stored or returned, keeps the conservative default.
        bool bEscapes = true;
    };

    [[nodiscard]] SEMA_EXPORT ClosureEnvFrame SynthesizeClosureFrame ( const ScopeTable &Scopes,
                                                                       const UnitTypes &Types,
                                                                       ScopeId ClosureScope );

} // namespace Sema

} // namespace Volt
