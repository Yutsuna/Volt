#pragma once

// AbiVerifier.hpp — LayoutEngine against LLVM's own DataLayout.
//
// LayoutEngine is the single ABI authority (.agents/backend/abi.md) and every
// target reads its numbers; LLVM computes its own from the DataLayout. They
// must agree, and a divergence is silent by nature — it corrupts an
// @[External] C struct field by field with no diagnostic anywhere. So it is
// checked, not assumed, on every aggregate the emitter mints.

#include "Core/EmitterServices.hpp"
#include "Core/LlvmFwd.hpp"

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class AbiVerifier
        {

        public:

            explicit AbiVerifier ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            // In debug builds, check that LLVM's own DataLayout agrees with
            // LayoutEngine about this aggregate's size and every field offset.
            // A no-op under NDEBUG.
            void VerifyAggregateAbi ( MiddleEnd::TypeSystem::LayoutId Id, llvm::StructType *Shape );

        private:

            EmitterServices *Services = nullptr;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
