#pragma once

// AbiClassifier.hpp — by value or by address, decided once for every target.
//
// abi.md fixes one rule for all three backends: a scalar travels in a register
// or a stack slot, an aggregate travels as the address of its storage ("in by
// pointer, out by value"). The decision reads the LayoutNode alone — no Volt
// type name, no target type, no DataLayout — so LLVM, WASM and any future
// target answer it identically instead of each re-deriving it next to its own
// type mapper.

#include "BackendCore_export.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <cstdint>

namespace Volt
{

namespace Backend
{

    // How a parameter or a return value of this shape crosses a call boundary.
    enum class EParamClass : std::uint8_t
    {

        Scalar    = 0, // register / stack slot
        ByAddress = 1, // aggregate: the parameter *is* the storage's address
    };

    [[nodiscard]] BACKENDCORE_EXPORT EParamClass ClassifyParam ( const MiddleEnd::TypeSystem::TypeStore &Store,
                                                                 MiddleEnd::TypeSystem::LayoutId Id );

    // The same fact under the name every emitter already asks it by. An
    // aggregate never sits in a register, so an expression of aggregate layout
    // evaluates to a pointer at its storage rather than to a loaded value.
    [[nodiscard]] BACKENDCORE_EXPORT bool IsAggregate ( const MiddleEnd::TypeSystem::TypeStore &Store,
                                                        MiddleEnd::TypeSystem::LayoutId Id );

} // namespace Backend

} // namespace Volt
