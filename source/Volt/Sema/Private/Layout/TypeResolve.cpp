// TypeResolve.cpp — the bridge between a declared signature and a concrete
// expression type.

#include "Volt/Sema/Layout/TypeResolve.hpp"

namespace Volt
{

namespace Sema
{

    SemaTypeId Instantiate (
        const TypeStore &Store, SigTypeId Id, std::span<const SemaTypeId> ReceiverArgs, SemaTypeId Self, UnitTypes &Values )
    {
        if ( not Id.IsValid() )
        {
            return SemaTypeId{};
        }

        const SigType &Sig = Store.Sig( Id );

        // `self` is the receiver itself — that is the whole point of writing
        // it rather than a name, and it is what makes one `min( other : self )`
        // in a mixin serve every type that includes it.
        if ( Sig.ParamIndex == SigType::SelfParam )
        {
            return Self;
        }

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
            Args.PushBack( Instantiate( Store, Arg, ReceiverArgs, Self, Values ) );
        }
        return Values.Intern( SemaType{ .Base = Sig.Base, .Args = std::move( Args ) } );
    }

    void UnifySig (
        const TypeStore &Store, const UnitTypes &Values, SigTypeId Pattern, SemaTypeId Actual, std::span<SemaTypeId> Bindings )
    {
        if ( not Pattern.IsValid() or not Values.Has( Actual ) )
        {
            return;
        }

        const SigType &Sig = Store.Sig( Pattern );

        // `self` is decided by the receiver, never by an argument: there is
        // nothing an actual type can teach about it.
        if ( Sig.ParamIndex == SigType::SelfParam )
        {
            return;
        }

        // A free parameter is the point of the walk. Already-bound slots —
        // everything the receiver supplied — win over what an argument
        // suggests, so a wrong argument stays a diagnostic rather than
        // silently redefining T.
        if ( Sig.ParamIndex >= 0 )
        {
            const auto Index = static_cast<std::size_t>( Sig.ParamIndex );
            if ( Index < Bindings.size() and not Bindings[Index].IsValid() )
            {
                Bindings[Index] = Actual;
            }
            return;
        }

        // Structural descent, but only where the two agree on the nominal:
        // matching `Array<U>` against a String has nothing to say about U.
        const SemaType &Concrete = Values.Get( Actual );
        if ( Concrete.Base != Sig.Base )
        {
            return;
        }
        for ( std::size_t Index = 0; Index < Sig.Args.Size() and Index < Concrete.Args.Size(); ++Index )
        {
            UnifySig( Store, Values, Sig.Args[Index], Concrete.Args[Index], Bindings );
        }
    }

    InstantiatedMember LookupMemberOn ( const TypeStore &Store,
                                        UnitTypes &Values,
                                        SemaTypeId Receiver,
                                        SemaTypeId Self,
                                        std::string_view Name,
                                        std::uint32_t Depth )
    {
        // Same bound as TypeStore::LookupMember: a malformed cyclic
        // hierarchy must not hang sema.
        if ( not Values.Has( Receiver ) or Depth > 16 )
        {
            return InstantiatedMember{};
        }

        // Copied, not referenced: Instantiate() interns into Values below,
        // which may move the arena out from under a SemaType reference.
        const SemaType Concrete = Values.Get( Receiver );
        if ( not Concrete.Base.IsValid() )
        {
            return InstantiatedMember{};
        }

        if ( const Member *Own = Store.OwnMember( Concrete.Base, Name ); Own != nullptr )
        {
            return InstantiatedMember{ .Decl = Own, .Owner = Receiver };
        }

        const std::span<const SemaTypeId> Applied{ Concrete.Args.begin(), Concrete.Args.Size() };

        // Mixins before the superclass, matching TypeStore::LookupMember —
        // the two must agree on which declaration wins, or a name would
        // exist on one path and type on the other.
        const NominalType &Type              = Store.Type( Concrete.Base );
        Core::SmallVec<SigTypeId, 2> Parents = Type.Includes;
        if ( Type.Super.IsValid() )
        {
            Parents.PushBack( Type.Super );
        }

        for ( const SigTypeId Parent : Parents )
        {
            const SemaTypeId Instance = Instantiate( Store, Parent, Applied, Self, Values );
            if ( const InstantiatedMember Found = LookupMemberOn( Store, Values, Instance, Self, Name, Depth + 1 );
                 Found.Decl != nullptr )
            {
                return Found;
            }
        }
        return InstantiatedMember{};
    }

} // namespace Sema

} // namespace Volt
