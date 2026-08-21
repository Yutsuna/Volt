#pragma once

// CalleeMap.hpp — the per-unit callee resolutions a backend consumes.

#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndIR_export.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Volt
{

namespace Meta
{
    class Writer;
    class Reader;
} // namespace Meta

namespace MiddleEnd
{

    namespace TypeSystem
    {
        using Member     = Volt::MiddleEnd::TypeSystem::Member;
        using TypeStore  = Volt::MiddleEnd::TypeSystem::TypeStore;
        using SemaTypeId = Volt::MiddleEnd::TypeSystem::SemaTypeId;
    } // namespace TypeSystem

    namespace IR
    {

        // The machine conversion an abstract member on a Pointer/Primitive
        // receiver maps to, decided by MemberResolver from the receiver's
        // LayoutKind and the member's signature shape — never by name.
        enum class EMachineConversion : std::uint8_t
        {
            None,          // ordinary call (has a body, or is a builtin op)
            PtrToInt,      // instance, 0 params → ptrtoint self
            IntToPtr,      // static,   1 param  → inttoptr arg[0]
            ScalarConvert, // instance, 0 params → convert primitive/pointer scalar to primitive/pointer scalar
        };

        // One resolved callee, with its already-instantiated signature.
        struct CalleeEntry
        {
            const TypeSystem::Member *Decl = nullptr;
            TypeSystem::SemaTypeId Result;
            ::Volt::Core::SmallVec<TypeSystem::SemaTypeId, 4> Params;
            TypeSystem::SemaTypeId BlockParam;
            ::Volt::Core::SmallVec<TypeSystem::SemaTypeId, 2> Bindings;
            TypeSystem::SemaTypeId Receiver;
            bool bConstructs                     = false;
            bool bIndirect                       = false;
            std::uint32_t VTableSlot             = 0;
            bool bDynamicDispatch                = false;
            EMachineConversion MachineConversion = EMachineConversion::None;
        };

        // Every callee one compile unit resolved, keyed by the callee expression's ExprId.
        class VOLT_MIDDLEEND_IR_EXPORT UnitCallees
        {

        public:

            void Set ( Frontend::ExprId Expr, CalleeEntry Entry )
            {
                if ( Expr.IsValid() )
                {
                    Entries[Expr.Value] = std::move( Entry );
                }
            }

            [[nodiscard]] const CalleeEntry *Get ( Frontend::ExprId Expr ) const
            {
                if ( not Expr.IsValid() )
                {
                    return nullptr;
                }
                const auto It = Entries.find( Expr.Value );
                return It != Entries.end() ? &It->second : nullptr;
            }

            [[nodiscard]] bool Has ( Frontend::ExprId Expr ) const
            {
                return Get( Expr ) != nullptr;
            }

            [[nodiscard]] std::size_t Size () const
            {
                return Entries.size();
            }

            void SerializeCache ( Meta::Writer &W ) const;
            [[nodiscard]] bool DeserializeCache ( Meta::Reader &R );

            void FixupDecls ( const TypeSystem::TypeStore &Store );

        private:

            std::unordered_map<std::uint32_t, CalleeEntry> Entries;
            std::vector<std::pair<std::uint32_t, std::pair<std::uint32_t, Frontend::DeclId>>> PendingDecls;
        };

    } // namespace IR

} // namespace MiddleEnd

} // namespace Volt
