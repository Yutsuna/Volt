#pragma once

#include "BackendCore_export.hpp"
#include "Volt/BackendCore/Mangler.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace Volt::Backend
{

struct VTableSlot
{
    const MiddleEnd::TypeSystem::Member *Decl = nullptr;
    bool bFinalize                            = false;
};

struct VTableDefinition
{
    MiddleEnd::TypeSystem::NominalId Concrete;
    MiddleEnd::TypeSystem::NominalId Trait;
    std::string SymbolName;
    std::vector<VTableSlot> Slots;
};

class BACKENDCORE_EXPORT VTableEngine
{

public:

    explicit VTableEngine ( const MiddleEnd::TypeSystem::TypeStore &InStore ) : Store( &InStore )
    {
    }

    [[nodiscard]] const VTableDefinition &GetDefinition ( MiddleEnd::TypeSystem::NominalId Concrete,
                                                          MiddleEnd::TypeSystem::NominalId Trait );

private:

    const MiddleEnd::TypeSystem::TypeStore *Store = nullptr;
    std::unordered_map<std::string, VTableDefinition> Cache;
};

} // namespace Volt::Backend
