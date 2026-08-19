#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <vector>

namespace Volt
{

namespace MiddleEnd::TypeSystem
{

    void TypeStore::ComputeSubtypeIntervals ()
    {
        const std::size_t Count = Types.Size();
        if ( Count == 0 )
        {
            return;
        }

        std::vector<std::vector<NominalId>> Children( Count );
        std::vector<bool> HasSuper( Count, false );

        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            const NominalId Id{ static_cast<NominalId::ValueType>( Index ) };
            const NominalId Super = BaseOf( Types.Get( Id ).Super );
            if ( Super.IsValid() and Super.Value < Count and Super != Id )
            {
                Children[Super.Value].push_back( Id );
                HasSuper[Index] = true;
            }
        }

        std::uint32_t Counter = 0;
        std::vector<bool> Visited( Count, false );

        auto Dfs = [&] ( auto &Self, NominalId Current ) -> void
        {
            if ( Current.Value >= Count or Visited[Current.Value] )
            {
                return;
            }
            Visited[Current.Value]   = true;
            const std::uint32_t Left = ++Counter;

            for ( const NominalId Child : Children[Current.Value] )
            {
                if ( Child.Value < Count and not Visited[Child.Value] )
                {
                    Self( Self, Child );
                }
            }
            const std::uint32_t Right          = Counter;
            Types.Get( Current ).PreorderLeft  = Left;
            Types.Get( Current ).PreorderRight = Right;
        };

        // 1. Visit roots (types with no super)
        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            if ( not HasSuper[Index] and not Visited[Index] )
            {
                Dfs( Dfs, NominalId{ static_cast<NominalId::ValueType>( Index ) } );
            }
        }

        // 2. Visit any disconnected/cyclic nodes
        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            if ( not Visited[Index] )
            {
                Dfs( Dfs, NominalId{ static_cast<NominalId::ValueType>( Index ) } );
            }
        }
    }

} // namespace MiddleEnd::TypeSystem

} // namespace Volt
