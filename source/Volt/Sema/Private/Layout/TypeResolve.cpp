// TypeResolve.cpp — the bridge between a declared signature and a concrete
// expression type.

#include "Volt/Sema/Layout/TypeResolve.hpp"

namespace Volt
{

namespace Sema
{

    SemaTypeId Instantiate ( const TypeStore &Store, SigTypeId Id, std::span<const SemaTypeId> ReceiverArgs, UnitTypes &Values )
    {
        if ( not Id.IsValid() )
        {
            return SemaTypeId{};
        }

        const SigType &Sig = Store.Sig( Id );

        // A parameter reference is answered positionally by the receiver;
        // no receiver argument means the generic was never instantiated.
        if ( Sig.ParamIndex >= 0 )
        {
            const auto Index = static_cast<std::size_t>( Sig.ParamIndex );
            return Index < ReceiverArgs.size() ? ReceiverArgs[Index] : SemaTypeId{};
        }

        Core::SmallVec<SemaTypeId, 2> Args;
        for ( const SigTypeId Arg : Sig.Args )
        {
            Args.PushBack( Instantiate( Store, Arg, ReceiverArgs, Values ) );
        }
        return Values.Intern( SemaType{ .Base = Sig.Base, .Args = std::move( Args ) } );
    }

} // namespace Sema

} // namespace Volt
