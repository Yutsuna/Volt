#pragma once

#include "Core/EmitterServices.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <llvm/IR/GlobalVariable.h>
#include <string>
#include <unordered_map>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        // Builds and caches global constant VTables: `@_VTable_<Concrete>_<Trait>`.
        // Slot 0 is always `finalize` (dynamic drop_in_place).
        // Slots 1..N are function pointers to the concrete implementations of Trait's methods.
        class VTableRegistry
        {

        public:

            explicit VTableRegistry ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            [[nodiscard]] llvm::GlobalVariable *GetOrCreateVTable ( MiddleEnd::TypeSystem::NominalId Concrete,
                                                                    MiddleEnd::TypeSystem::NominalId Trait );

        private:

            EmitterServices *Services;
            std::unordered_map<std::string, llvm::GlobalVariable *> Cache;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
