// SemaTypeSerialize.cpp — UnitTypes::SerializeCache/DeserializeCache.
//
// Only the per-unit *mappings* travel here. The types themselves belong to the
// build's `TypeUniverse`, persisted once by `TypeStore::SerializeCache` and
// replayed in intern order — so every SemaTypeId below comes back meaning what
// it meant when written, with no remap table (see TypeUniverse.hpp).

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"

namespace Volt
{

namespace MiddleEnd
{

    namespace TypeSystem
    {

        void UnitTypes::SerializeCache ( Meta::Writer &W ) const
        {
            Meta::Serialize( W, OfExpr );
            Meta::Serialize( W, Deferred );
            Meta::Serialize( W, SiteTypes );
        }

        bool UnitTypes::DeserializeCache ( Meta::Reader &R )
        {
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

            // A pure fast path over the universe, never storage: nothing read
            // above needs it, and it refills itself on the first Intern this
            // unit performs after the replay.
            Memo.clear();
            return true;
        }

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
