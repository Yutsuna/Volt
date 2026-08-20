// SynthesizedFunctionsSerialize.cpp — SynthesizedFunctions::SerializeCache /
// DeserializeCache.
//
// Symmetric with CalleeMapSerialize.cpp, and simpler for one reason: a
// SynthesizedFunction holds no `Member *`, so there is no two-phase fixup —
// its `Decl` indexes the unit's own AST arena, which the same cache restores.

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/MiddleEnd/IR/SynthesizedFunctions.hpp"

namespace Volt
{

namespace MiddleEnd
{

    namespace IR
    {

        void SynthesizedFunctions::SerializeCache ( Meta::Writer &W ) const
        {
            const auto Count = static_cast<std::uint32_t>( Entries.size() );
            Meta::Serialize( W, Count );
            for ( const SynthesizedFunction &Entry : Entries )
            {
                Meta::Serialize( W, Entry.Decl );
                Meta::Serialize( W, Entry.Result );
                Meta::Serialize( W, Entry.Params );
            }
        }

        bool SynthesizedFunctions::DeserializeCache ( Meta::Reader &R )
        {
            std::uint32_t Count = 0;
            if ( not Meta::Deserialize( R, Count ) )
            {
                return false;
            }

            Entries.clear();
            Entries.reserve( Count );

            for ( std::uint32_t Index = 0; Index < Count; ++Index )
            {
                SynthesizedFunction Entry;
                if ( not Meta::Deserialize( R, Entry.Decl ) or not Meta::Deserialize( R, Entry.Result ) or
                     not Meta::Deserialize( R, Entry.Params ) )
                {
                    return false;
                }
                Entries.push_back( std::move( Entry ) );
            }
            return true;
        }

    } // namespace IR

} // namespace MiddleEnd

} // namespace Volt
