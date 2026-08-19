#include "Volt/BackendCore/VTableLayout.hpp"

const Volt::Backend::VTableDefinition &Volt::Backend::VTableEngine::GetDefinition ( MiddleEnd::TypeSystem::NominalId Concrete,
                                                                                    MiddleEnd::TypeSystem::NominalId Trait )
{
    static const VTableDefinition EmptyDef{};
    if ( not Concrete.IsValid() or not Trait.IsValid() or Store == nullptr )
    {
        return EmptyDef;
    }

    const std::string ConcreteMangled = MangleNominal( *Store, Concrete );
    const std::string TraitMangled    = MangleNominal( *Store, Trait );
    const std::string VTableName      = "_VTable_" + ConcreteMangled + "_" + TraitMangled;

    if ( const auto It = Cache.find( VTableName ); It != Cache.end() )
    {
        return It->second;
    }

    VTableDefinition Def;
    Def.Concrete   = Concrete;
    Def.Trait      = Trait;
    Def.SymbolName = VTableName;

    const auto Slots = Store->VTableSlotsOf( Trait );
    Def.Slots.reserve( 1 + Slots.size() );

    // Slot 0: Finalize (drop_in_place)
    const bool bTrivial = Store->Type( Concrete ).bTrivialFinalize;
    if ( bTrivial )
    {
        Def.Slots.push_back( VTableSlot{ .Decl = nullptr, .bFinalize = true } );
    }
    else
    {
        const auto *FinDecl = Store->OwnMember( Concrete, "finalize" );
        Def.Slots.push_back( VTableSlot{ .Decl = FinDecl, .bFinalize = true } );
    }

    // Slots 1..N: Virtual methods in declaration order
    for ( const auto MethodSym : Slots )
    {
        const std::string_view MethodName = Store->Text( MethodSym );
        const auto Found                  = Store->LookupMember( Concrete, MethodName );
        Def.Slots.push_back( VTableSlot{ .Decl = Found.Decl, .bFinalize = false } );
    }

    auto [It, _] = Cache.emplace( VTableName, std::move( Def ) );
    return It->second;
}
