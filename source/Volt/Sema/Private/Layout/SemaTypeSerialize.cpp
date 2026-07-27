// SemaTypeSerialize.cpp — UnitTypes::SerializeCache/DeserializeCache.

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"

namespace Volt
{

namespace Sema
{

    void UnitTypes::SerializeCache ( Meta::Writer &W ) const
    {
        Meta::SerializeArena( W, Types );
        Meta::Serialize( W, OfExpr );
        Meta::Serialize( W, Deferred );
        Meta::Serialize( W, SiteTypes );
    }

    bool UnitTypes::DeserializeCache ( Meta::Reader &R )
    {
        if ( not Meta::DeserializeArena( R, Types ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, OfExpr ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, Deferred ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, SiteTypes ) )
        {
            return false;
        }

        Dedup.clear();
        for ( std::size_t Index = 0; Index < Types.Size(); ++Index )
        {
            const SemaTypeId Id{ static_cast<std::uint32_t>( Index ) };
            const SemaType &Value = Types.Get( Id );

            std::vector<std::uint32_t> Key;
            Key.reserve( 1 + Value.Args.Size() );
            Key.push_back( Value.Base.Value );
            for ( const SemaTypeId Arg : Value.Args )
            {
                Key.push_back( Arg.Value );
            }
            Dedup.emplace( std::move( Key ), Id );
        }
        return true;
    }

} // namespace Sema

} // namespace Volt
