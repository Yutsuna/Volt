#pragma once

#include "Sema_export.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Scope/ScopeTable.hpp"

#include <cstddef>
#include <cstdint>

namespace Volt
{

namespace Sema
{

    struct ClosureEnvField
    {
        Symbol Name;
        BindingSite Site;
        SemaTypeId Type;
        std::size_t Offset = 0;
    };

    struct ClosureEnvFrame
    {
        ScopeId Scope;
        std::size_t TotalSize = 0;
        std::size_t Alignment = 1;
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
