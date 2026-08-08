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

    std::optional<BoundViolation>
    CheckGenericBounds ( const TypeStore &Store, UnitTypes &Values, NominalId Base, std::span<const SemaTypeId> Args )
    {
        if ( not Base.IsValid() )
        {
            return std::nullopt;
        }

        const NominalType &Type = Store.Type( Base );

        for ( std::size_t Index = 0; Index < Type.ParamBounds.Size() and Index < Args.size(); ++Index )
        {
            const SigTypeId BoundSig = Type.ParamBounds[Index];
            if ( not BoundSig.IsValid() )
            {
                continue; // unbounded parameter
            }

            const NominalId Bound = Store.BaseOf( BoundSig );
            if ( not Bound.IsValid() )
            {
                continue;
            }

            const SemaTypeId Arg = Args[Index];
            if ( not Values.Has( Arg ) )
            {
                continue; // deferred — nothing concrete to check yet
            }

            const NominalId ArgBase = Values.Get( Arg ).Base;

            // Satisfied the same way CheckAbstractConformance answers "does
            // this includer satisfy this mixin": every abstract member the
            // bound declares directly must resolve to a non-abstract
            // implementation on the supplied argument.
            bool bSatisfied = true;
            for ( const Member &Entry : Store.Type( Bound ).Members )
            {
                if ( not Entry.bAbstract )
                {
                    continue;
                }
                const InstantiatedMember Found = LookupMemberOn( Store, Values, Arg, Arg, Store.Text( Entry.Name ) );
                if ( Found.Decl == nullptr or Found.Decl->bAbstract )
                {
                    bSatisfied = false;
                    break;
                }
            }

            if ( not bSatisfied )
            {
                return BoundViolation{ .ParamIndex = Index, .RequiredBound = Bound, .SuppliedBase = ArgBase };
            }
        }

        return std::nullopt;
    }

    bool IsSubclassOf ( const TypeStore &Store, NominalId Child, NominalId Parent )
    {
        if ( not Child.IsValid() or not Parent.IsValid() )
        {
            return false;
        }
        NominalId Current   = Child;
        std::uint32_t Depth = 0;
        while ( Current.IsValid() and Depth < 100 )
        {
            if ( Current == Parent )
            {
                return true;
            }
            const NominalType &Type = Store.Type( Current );
            if ( not Type.Super.IsValid() )
            {
                break;
            }
            const SigType &SuperSig = Store.Sig( Type.Super );
            Current                 = SuperSig.Base;
            Depth++;
        }
        return false;
    }

} // namespace Sema

} // namespace Volt
