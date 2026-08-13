#pragma once

// LayoutEngine.hpp — the one ABI authority all three emitters consume.
//
// Sizes, alignments and field offsets are derived from LayoutNode alone
// (Primitive | Pointer | Aggregate): no Volt type name ever enters the
// computation (rules/zero-hardcode.md). Every target reads the same
// numbers, so a struct laid out by the VM matches what LLVM and WASM emit.

#include "BackendCore_export.hpp"
#include "Volt/MiddleEnd/TypeSystem/MemoryLayout.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <cstddef>

namespace Volt
{

namespace Backend
{

    struct SizeAlign
    {

        std::size_t Size      = 0;
        std::size_t Alignment = 1;
    };

    class BACKENDCORE_EXPORT LayoutEngine
    {

    public:

        explicit LayoutEngine ( const MiddleEnd::TypeSystem::TypeStore &InStore ) : Store( InStore )
        {
        }

        // Natural layout rules: a Primitive is Bits rounded up to whole
        // bytes, aligned to the smallest power of two holding it (capped at
        // 8); a Pointer is 8/8; an Aggregate lays fields out in declaration
        // order, each at its own alignment, padded to the max field
        // alignment.
        [[nodiscard]] SizeAlign Of ( MiddleEnd::TypeSystem::LayoutId Id ) const;

        // Byte offset of field `Index` inside an Aggregate layout. Returns 0
        // for anything that is not an aggregate or out of range — callers
        // index off the same LayoutNode they resolved the field on.
        [[nodiscard]] std::size_t FieldOffset ( MiddleEnd::TypeSystem::LayoutId Aggregate, std::size_t Index ) const;

    private:

        const MiddleEnd::TypeSystem::TypeStore &Store;
    };

} // namespace Backend

} // namespace Volt
