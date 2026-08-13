#pragma once

// CalleeMap.hpp — the per-unit callee resolutions a backend consumes.

#include "VoltMiddleEndIR_export.hpp"
#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

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
    using Member = Volt::Sema::Member;
    using TypeStore = Volt::Sema::TypeStore;
    using SemaTypeId = Volt::Sema::SemaTypeId;
} // namespace TypeSystem

namespace IR
{

    // One resolved callee, with its already-instantiated signature.
    struct CalleeEntry
    {
        const TypeSystem::Member *Decl = nullptr;
        TypeSystem::SemaTypeId Result;
        Core::SmallVec<TypeSystem::SemaTypeId, 4> Params;
        TypeSystem::SemaTypeId BlockParam;
        Core::SmallVec<TypeSystem::SemaTypeId, 2> Bindings;
        TypeSystem::SemaTypeId Receiver;
        bool bConstructs = false;
        bool bIndirect   = false;
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
