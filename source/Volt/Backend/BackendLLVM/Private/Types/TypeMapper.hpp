#pragma once

// TypeMapper.hpp — LayoutId -> llvm::Type*, and the layout of an expression.
//
// The mapping reads a layout and nothing else: Primitive{ Spelling, Bits },
// Pointer, Aggregate{ Fields }. `"i32"` and `"f64"` are opaque interned strings
// that select a *machine* shape; the compiler never learns which Volt type
// wrote them (rules/zero-hardcode.md). Signedness is deliberately absent here —
// it lives in the instruction, not in the type.

#include "Core/EmitterServices.hpp"
#include "Core/LlvmFwd.hpp"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        struct FunctionFrame;

        class TypeMapper
        {

        public:

            explicit TypeMapper ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            // The LLVM type for a memory layout. Null when the layout is
            // unresolved.
            [[nodiscard]] llvm::Type *TypeOfLayout ( Sema::LayoutId Id );

            // How a value of this layout crosses a call boundary. Scalars
            // travel in a register; an aggregate travels by pointer, which is
            // what abi.md fixes for all three targets. Null when unresolved.
            [[nodiscard]] llvm::Type *ParamTypeOfLayout ( Sema::LayoutId Id );

            // The MonoRequest encoding of an inferred expression type: a
            // pre-order walk emitting, per node, its NominalId then its own
            // argument count. The one currency InstanceLayouts, Monomorphizer
            // and Mangler share, so a layout, a symbol and a queue entry can
            // never mean different instantiations.
            void
            FlattenValueType ( const Sema::UnitTypes &Values, Sema::SemaTypeId Id, std::vector<std::uint32_t> &Out ) const;

            // The memory shape of a value of this inferred type. Invalid when
            // the type is absent — which inside a generic body is normal
            // (UnitTypes::IsDeferred) and everywhere else is a middle-end hole
            // the caller reports.
            [[nodiscard]] Sema::LayoutId LayoutOfValue ( const Sema::UnitTypes &Values, Sema::SemaTypeId Id );

            // The layout, then the llvm::Type, of what an expression evaluates
            // to. Both read the frame's own Values overlay, which is the unit's
            // for a concrete body and ReinstantiateBody's for a monomorphised
            // one.
            [[nodiscard]] Sema::LayoutId LayoutOfExpr ( const FunctionFrame &Frame, Frontend::ExprId Id );
            [[nodiscard]] llvm::Type *TypeOfExpr ( const FunctionFrame &Frame, Frontend::ExprId Id );

            // An aggregate never travels in a register (abi.md): it is
            // addressed, so every expression of aggregate layout evaluates to a
            // `ptr` at its storage rather than to a loaded struct value.
            [[nodiscard]] bool IsAggregate ( Sema::LayoutId Id ) const;

            // The opaque spelling driving instruction selection for a layout: a
            // Primitive's own, and "ptr" for a Pointer, so the two shapes of
            // address cannot select different instructions. Empty for an
            // aggregate.
            [[nodiscard]] std::string_view SpellingOf ( Sema::LayoutId Id ) const;

        private:

            EmitterServices *Services = nullptr;
            // LayoutId -> llvm::Type*. One entry per distinct memory shape, so
            // an aggregate is structurally created once no matter how many
            // types share it.
            std::unordered_map<std::uint32_t, llvm::Type *> Cache;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
