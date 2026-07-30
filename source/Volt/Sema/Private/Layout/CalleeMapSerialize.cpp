// CalleeMapSerialize.cpp — UnitCallees::SerializeCache/DeserializeCache and
// the CalleeEntry::Decl two-phase fixup (rules/ast-value.md-adjacent: a raw
// pointer into another arena-backed store is never itself serialisable).

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Sema/Layout/CalleeMap.hpp"

namespace Volt
{

namespace Sema
{

    void UnitCallees::SerializeCache ( Meta::Writer &W ) const
    {
        const auto Count = static_cast<std::uint32_t>( Entries.size() );
        Meta::Serialize( W, Count );
        for ( const auto &[Key, Entry] : Entries )
        {
            Meta::Serialize( W, Key );

            const bool bHasDecl = Entry.Decl != nullptr;
            Meta::Serialize( W, bHasDecl );
            if ( bHasDecl )
            {
                Meta::Serialize( W, Entry.Decl->Unit );
                Meta::Serialize( W, Entry.Decl->Decl );
            }

            Meta::Serialize( W, Entry.Result );
            Meta::Serialize( W, Entry.Params );
            Meta::Serialize( W, Entry.BlockParam );
            Meta::Serialize( W, Entry.Bindings );
            Meta::Serialize( W, Entry.Receiver );
            Meta::Serialize( W, Entry.bConstructs );
            Meta::Serialize( W, Entry.bIndirect );
        }
    }

    bool UnitCallees::DeserializeCache ( Meta::Reader &R )
    {
        std::uint32_t Count = 0;
        if ( not Meta::Deserialize( R, Count ) )
        {
            return false;
        }

        Entries.clear();
        PendingDecls.clear();
        Entries.reserve( Count );

        for ( std::uint32_t Index = 0; Index < Count; ++Index )
        {
            std::uint32_t Key = 0;
            if ( not Meta::Deserialize( R, Key ) )
            {
                return false;
            }

            bool bHasDecl = false;
            if ( not Meta::Deserialize( R, bHasDecl ) )
            {
                return false;
            }
            if ( bHasDecl )
            {
                std::uint32_t Unit = 0;
                Frontend::DeclId Decl;
                if ( not Meta::Deserialize( R, Unit ) or not Meta::Deserialize( R, Decl ) )
                {
                    return false;
                }
                PendingDecls.emplace_back( Key, std::make_pair( Unit, Decl ) );
            }

            CalleeEntry Entry;
            if ( not Meta::Deserialize( R, Entry.Result ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, Entry.Params ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, Entry.BlockParam ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, Entry.Bindings ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, Entry.Receiver ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, Entry.bConstructs ) )
            {
                return false;
            }
            if ( not Meta::Deserialize( R, Entry.bIndirect ) )
            {
                return false;
            }
            Entries.emplace( Key, std::move( Entry ) );
        }
        return true;
    }

    void UnitCallees::FixupDecls ( const TypeStore &Store )
    {
        for ( const auto &[Key, UnitDecl] : PendingDecls )
        {
            const auto It = Entries.find( Key );
            if ( It == Entries.end() )
            {
                continue;
            }
            It->second.Decl = Store.FindMemberByUnitDecl( UnitDecl.first, UnitDecl.second );
        }
        PendingDecls.clear();
    }

} // namespace Sema

} // namespace Volt
