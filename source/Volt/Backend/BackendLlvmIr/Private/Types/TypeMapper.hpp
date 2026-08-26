#pragma once

// TypeMapper.hpp — LayoutId -> llvm::Type*, and the layout of an expression.
//
// The mapping reads a layout and nothing else: Primitive{ Spelling, Bits },
// Pointer, Aggregate{ Fields }. `"i32"` and `"f64"` are opaque interned strings
// that select a *machine* shape; the compiler never learns which Volt type
// wrote them (rules/zero-hardcode.md). Signedness is deliberately absent here —
// it lives in the instruction, not in the type.

#include "Volt/BackendCore/AbiClassifier.hpp"

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
            [[nodiscard]] llvm::Type *TypeOfLayout ( MiddleEnd::TypeSystem::LayoutId Id );

            // The same layout as one byte-addressable copy of itself. A machine
            // addresses whole bytes, so a primitive declared narrower than a
            // byte still occupies one wherever it is stored, and the register
            // type is not the type that byte has.
            //
            // The distinction only bites where the same bytes are reachable two
            // ways. An aggregate is assigned by `memcpy`, SROA splits that copy
            // at byte granularity, and a field it also sees written at the
            // declared width is a field it cannot reconcile: the slot survives
            // as an `alloca`, unpromoted, and so does every branch reading it.
            // So aggregate fields are storage-typed, and the two ends of a field
            // access — EmitStore and LoadPlace — widen and narrow around them.
            //
            // Read off the declared width and nothing else, like everything else
            // here: a width already a whole number of bytes is its own storage
            // type (rules/zero-hardcode.md).
            [[nodiscard]] llvm::Type *StorageTypeOfLayout ( MiddleEnd::TypeSystem::LayoutId Id );

            // How a value of this layout crosses a call boundary. Scalars
            // travel in a register; an aggregate travels by pointer, which is
            // what abi.md fixes for all three targets. Null when unresolved.
            [[nodiscard]] llvm::Type *ParamTypeOfLayout ( MiddleEnd::TypeSystem::LayoutId Id );

            // Both of these are BackendCore's (InstanceLayout.hpp): the
            // SemaTypeId -> LayoutId bridge is the MonoRequest currency, shared
            // by every target, and no emitter may reach it by a route of its
            // own. They stay reachable through the mapper because that is where
            // a body already has one in hand.
            void FlattenValueType ( const MiddleEnd::TypeSystem::UnitTypes &Values,
                                    MiddleEnd::TypeSystem::SemaTypeId Id,
                                    std::vector<std::uint32_t> &Out ) const
            {
                Backend::FlattenValueType( *Services->Build, Values, Id, Out );
            }

            [[nodiscard]] MiddleEnd::TypeSystem::LayoutId LayoutOfValue ( const MiddleEnd::TypeSystem::UnitTypes &Values,
                                                                          MiddleEnd::TypeSystem::SemaTypeId Id )
            {
                return Backend::LayoutOfValue( *Services->Build, *Services->Instances, Values, Id );
            }

            // The layout, then the llvm::Type, of what an expression evaluates
            // to. Both read the frame's own Values overlay, which is the unit's
            // for a concrete body and ReinstantiateBody's for a monomorphised
            // one.
            [[nodiscard]] MiddleEnd::TypeSystem::LayoutId LayoutOfExpr ( const FunctionFrame &Frame, Frontend::ExprId Id );
            [[nodiscard]] llvm::Type *TypeOfExpr ( const FunctionFrame &Frame, Frontend::ExprId Id );

            // An aggregate never travels in a register (abi.md): it is
            // addressed, so every expression of aggregate layout evaluates to a
            // `ptr` at its storage rather than to a loaded struct value. The
            // decision itself is BackendCore's (AbiClassifier.hpp) — all three
            // targets read the same answer.
            [[nodiscard]] bool IsAggregate ( MiddleEnd::TypeSystem::LayoutId Id ) const;

            // The opaque spelling driving instruction selection for a layout: a
            // Primitive's own, and "ptr" for a Pointer, so the two shapes of
            // address cannot select different instructions. Empty for an
            // aggregate.
            [[nodiscard]] std::string_view SpellingOf ( MiddleEnd::TypeSystem::LayoutId Id ) const;

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
