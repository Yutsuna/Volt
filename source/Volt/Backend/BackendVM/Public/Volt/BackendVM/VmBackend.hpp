#pragma once

// VmBackend.hpp — the TargetBackend that lowers core AST to VM bytecode.
// Skeleton: emission is specified in .agents/backend/vm.md; EmitUnit reports
// Unimplemented rather than pretending.

#include "BackendVM_export.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"
#include "Volt/BackendVM/Bytecode.hpp"

#include <string_view>

namespace Volt
{

namespace Backend
{

    namespace VM
    {

        class BACKENDVM_EXPORT VmBackend
        {

        public:

            [[nodiscard]] std::string_view Name () const
            {
                return "vm";
            }

            void Begin ( const BackendInput &Input );

            [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit );

            [[nodiscard]] EmitResult Finalize ();

        private:

            const BackendInput *Build = nullptr;
            FunctionTable Functions;
        };

    } // namespace VM

} // namespace Backend

} // namespace Volt
