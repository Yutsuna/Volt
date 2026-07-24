#pragma once

// LlvmEmitter.hpp — the TargetBackend behind `volt build` (native AOT).
//
// No LLVM header leaks out of this module: the public surface is plain
// Volt types and the LLVM state hides behind a pimpl, so nothing upstream
// recompiles against the LLVM API and the module stays optional
// (VOLT_ENABLE_LLVM). Emission is specified in .agents/backend/llvm.md.

#include "BackendLLVM_export.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"

#include <memory>
#include <string_view>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class BACKENDLLVM_EXPORT LlvmBackend
        {

        public:

            LlvmBackend ();
            ~LlvmBackend ();
            LlvmBackend ( LlvmBackend && ) noexcept;
            LlvmBackend &operator=( LlvmBackend && ) noexcept;

            [[nodiscard]] std::string_view Name () const
            {
                return "llvm";
            }

            void Begin ( const BackendInput &Input );

            [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit );

            [[nodiscard]] EmitResult Finalize ();

        private:

            struct State;
            std::unique_ptr<State> Impl;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
