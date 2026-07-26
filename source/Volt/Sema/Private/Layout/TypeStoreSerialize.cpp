// TypeStoreSerialize.cpp — TypeStore::SerializeCache/DeserializeCache.
//
// A hand-written pair, not a Meta::Reflected fallback: TypeStore is a class
// with private state and derived indexes (ByName, ByNodeKind, FunctionByName,
// Modules), exactly like Core::Arena/StringInterner themselves. The body is
// nothing but calls to the generic Serialize/SerializeArena/SerializeInterner
// primitives (rules/meta-first.md still applies inside it).

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

namespace Volt
{

namespace Sema
{

    void TypeStore::SerializeCache ( Meta::Writer &W ) const
    {
        Meta::SerializeInterner( W, Strings );
        Meta::SerializeArena( W, Types );
        Meta::SerializeArena( W, Sigs );
        Meta::SerializeArena( W, Layouts );
        Meta::Serialize( W, ByName );
        Meta::Serialize( W, ByNodeKind );
        Meta::Serialize( W, ExceptionRoot );
        Meta::Serialize( W, Functions );
        Meta::Serialize( W, FunctionByName );

        const auto ModuleCount = static_cast<std::uint32_t>( Modules.size() );
        Meta::Serialize( W, ModuleCount );
        for ( const Symbol Entry : Modules )
        {
            Meta::Serialize( W, Entry );
        }
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
        if ( not Meta::Deserialize( R, ExceptionRoot ) )
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
        return true;
    }

} // namespace Sema

} // namespace Volt
