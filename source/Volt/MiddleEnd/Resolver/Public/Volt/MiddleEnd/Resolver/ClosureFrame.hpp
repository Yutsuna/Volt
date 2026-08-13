#pragma once

#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "VoltMiddleEndResolver_export.hpp"

namespace Volt
{

namespace MiddleEnd::Resolver
{

    using TypeSystem::SemaTypeId;
    using TypeSystem::UnitTypes;

    struct ClosureEnvField
    {
        Symbol Name;
        BindingSite Site;
        SemaTypeId Type;
    };

    struct ClosureEnvFrame
    {
        ScopeId Scope;
        ::Volt::Core::SmallVec<ClosureEnvField, 4> Fields;

        bool bEscapes = true;
    };

    [[nodiscard]] VOLT_MIDDLEEND_RESOLVER_EXPORT ClosureEnvFrame SynthesizeClosureFrame ( const ScopeTable &Scopes,
                                                                                          const UnitTypes &Types,
                                                                                          ScopeId ClosureScope );

} // namespace MiddleEnd::Resolver

} // namespace Volt
