#pragma once

#include "Core/EmitterServices.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include "Core/LlvmFwd.hpp"

#include <string>
#include <unordered_map>
#include <vector>

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

            // Where every pointer this emission put in a vtable lives, for a
            // consumer that will later have to move one (IrGenerator::VTableEntry
            // says why nothing else can). Recorded under indirect linkage only.
            [[nodiscard]] const std::vector<Ir::IrGenerator::VTableEntry> &Entries () const
            {
                return Recorded;
            }

        private:

            EmitterServices *Services;
            std::unordered_map<std::string, llvm::GlobalVariable *> Cache;
            std::vector<Ir::IrGenerator::VTableEntry> Recorded;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
