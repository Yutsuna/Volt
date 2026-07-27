// ScopeTableSerialize.cpp — ScopeTable::SerializeCache/DeserializeCache.
//
// Every field but UseIndex round-trips through the generic Meta::Serialize
// machinery untouched (Scope/Binding/Capture are reflected aggregates,
// BindingSite is a std::variant of TypedIds already handled). UseIndex holds
// raw `const Binding*` into a Scope's own Bindings map, so it gets the same
// two-phase treatment as CalleeMap.hpp's Decl: written as (Owner, Name),
// re-resolved once Scopes itself has been replayed.

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Sema/Scope/ScopeTable.hpp"

namespace Volt
{

namespace Sema
{

    void ScopeTable::SerializeCache ( Meta::Writer &W ) const
    {
        Meta::SerializeArena( W, Scopes );

        const auto UseCount = static_cast<std::uint32_t>( UseIndex.size() );
        Meta::Serialize( W, UseCount );
        for ( const Binding *Entry : UseIndex )
        {
            const bool bHasEntry = Entry != nullptr;
            Meta::Serialize( W, bHasEntry );
            if ( bHasEntry )
            {
                Meta::Serialize( W, Entry->Owner );
                Meta::Serialize( W, Entry->Name );
            }
        }

        Meta::Serialize( W, StmtScope );
        Meta::Serialize( W, ExprScope );
        Meta::Serialize( W, ClosureCaptures );
        Meta::Serialize( W, ClosureEscapes );
        Meta::Serialize( W, UseCounts );
    }

    bool ScopeTable::DeserializeCache ( Meta::Reader &R )
    {
        if ( not Meta::DeserializeArena( R, Scopes ) )
        {
            return false;
        }

        std::uint32_t UseCount = 0;
        if ( not Meta::Deserialize( R, UseCount ) )
        {
            return false;
        }
        UseIndex.assign( UseCount, nullptr );
        for ( std::uint32_t Index = 0; Index < UseCount; ++Index )
        {
            bool bHasEntry = false;
            if ( not Meta::Deserialize( R, bHasEntry ) )
            {
                return false;
            }
            if ( not bHasEntry )
            {
                continue;
            }

            ScopeId Owner;
            Symbol Name;
            if ( not Meta::Deserialize( R, Owner ) or not Meta::Deserialize( R, Name ) )
            {
                return false;
            }
            if ( not Owner.IsValid() or Owner.Value >= Scopes.Size() )
            {
                return false;
            }

            const Scope &Target = Scopes.Get( Owner );
            const auto It       = Target.Bindings.find( Name );
            if ( It == Target.Bindings.end() )
            {
                return false;
            }
            UseIndex[Index] = &It->second;
        }

        if ( not Meta::Deserialize( R, StmtScope ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, ExprScope ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, ClosureCaptures ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, ClosureEscapes ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, UseCounts ) )
        {
            return false;
        }
        return true;
    }

} // namespace Sema

} // namespace Volt
