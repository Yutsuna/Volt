#include "Ownership.hpp"

namespace Volt::Sema::Raii
{

bool IsFinalizeCandidateNominal ( const TypeStore &Store, const NominalId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    const NominalType &Type = Store.Type( Id );
    if ( not Type.Layout.IsValid() or KindOf( Store.Get( Type.Layout ) ) != LayoutKind::Aggregate )
    {
        return false;
    }
    // Not "does it declare `finalize`" any more — since the seam gives every
    // type one, that question answers yes for everything. The question is
    // whether destroying a value of it *does* anything, which is the same
    // question C++ asks before emitting a destructor call.
    return not Type.bTrivialFinalize;
}

bool IsFinalizeCandidateType ( const TypeStore &Store, UnitTypes &Values, const SemaTypeId Type )
{
    if ( not Type.IsValid() or not Values.Has( Type ) )
    {
        return false;
    }
    return IsFinalizeCandidateNominal( Store, Values.Get( Type ).Base );
}

} // namespace Volt::Sema::Raii
