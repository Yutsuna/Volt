#pragma once

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
    };

    [[nodiscard]] ClosureEnvFrame
    SynthesizeClosureFrame ( const ScopeTable &Scopes, const UnitTypes &Types, ScopeId ClosureScope );

} // namespace Sema

} // namespace Volt
