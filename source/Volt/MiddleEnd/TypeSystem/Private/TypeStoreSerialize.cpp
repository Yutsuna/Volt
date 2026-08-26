// TypeStoreSerialize.cpp — TypeStore::SerializeCache/DeserializeCache.
//
// A hand-written pair, not a Meta::Reflected fallback: TypeStore is a class
// with private state and derived indexes (ByName, ByNodeKind, FunctionByName,
// Modules), exactly like Volt::Core::Arena/StringInterner themselves. The body is
// nothing but calls to the generic Serialize/SerializeArena/SerializeInterner
// primitives (rules/meta-first.md still applies inside it).

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeUniverse.hpp"

namespace Volt
{

namespace MiddleEnd
{

    namespace TypeSystem
    {

        void TypeStore::SerializeCache ( Meta::Writer &W ) const
        {
            Meta::SerializeInterner( W, Strings );
            Meta::SerializeArena( W, Types );
            Meta::SerializeArena( W, Sigs );
            Meta::SerializeArena( W, Layouts );
            Meta::Serialize( W, ByName );
            Meta::Serialize( W, ByNodeKind );
            Meta::Serialize( W, Functions );
            Meta::Serialize( W, FunctionByName );
            Meta::Serialize( W, Variables );
            Meta::Serialize( W, VariableByName );

            const auto ModuleCount = static_cast<std::uint32_t>( Modules.size() );
            Meta::Serialize( W, ModuleCount );
            for ( const Symbol Entry : Modules )
            {
                Meta::Serialize( W, Entry );
            }

            // The canonical expression types ride with the store, not with the
            // units: they are shared by all of them, and replaying them here —
            // before any unit's ExprId -> SemaTypeId mapping is read back —
            // is what makes those mappings meaningful without a remap
            // (TypeUniverse.hpp, "Determinism").
            UniverseStorage->SerializeCache( W );
        }

        bool TypeStore::DeserializeCache ( Meta::Reader &R )
        {
            if ( not Meta::DeserializeInterner( R, Strings ) )
            {
                return false;
            }
            if ( not Meta::DeserializeArena( R, Types ) )
            {
                return false;
            }
            if ( not Meta::DeserializeArena( R, Sigs ) )
            {
                return false;
            }
            if ( not Meta::DeserializeArena( R, Layouts ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, ByName ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, ByNodeKind ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, Functions ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, FunctionByName ) )
            {
                return false;
            }

            if ( not Meta::Deserialize( R, Variables ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, VariableByName ) )
            {
                return false;
            }

            std::uint32_t ModuleCount = 0;
            if ( not Meta::Deserialize( R, ModuleCount ) )
            {
                return false;
            }
            Modules.clear();
            Modules.reserve( ModuleCount );
            for ( std::uint32_t Index = 0; Index < ModuleCount; ++Index )
            {
                Symbol Entry;
                if ( not Meta::Deserialize( R, Entry ) )
                {
                    return false;
                }
                Modules.insert( Entry );
            }

            if ( not UniverseStorage->DeserializeCache( R ) )
            {
                return false;
            }

            RebuildSigIndex();
            return true;
        }

        void TypeStore::RebuildSigIndex ()
        {
            SigDedup.clear();
            SigDedup.reserve( Sigs.Size() );
            for ( std::size_t Index = 0; Index < Sigs.Size(); ++Index )
            {
                const SigTypeId Id{ static_cast<SigTypeId::ValueType>( Index ) };
                SigDedup.emplace( SigKeyOf( Sigs.Get( Id ) ), Id );
            }
        }

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
