// LayoutOfValue.cpp — the SemaTypeId half of TypeMapper.
//
// An *inferred* type (MiddleEnd::TypeSystem::SemaTypeId, per unit and per expression) becomes a
// *memory shape* (MiddleEnd::TypeSystem::LayoutId, build-wide) by being flattened into the
// MonoRequest encoding first — the one currency InstanceLayouts, Monomorphizer
// and Mangler share, so a layout, a symbol and a queue entry can never mean
// different instantiations. Split from TypeMapper.cpp because it is that
// currency's own concern and reads no llvm:: type at all.

#include "Types/TypeMapper.hpp"

#include <cstdint>
#include <span>
#include <vector>

void Volt::Backend::Llvm::TypeMapper::FlattenValueType ( const MiddleEnd::TypeSystem::UnitTypes &Values,
                                                         MiddleEnd::TypeSystem::SemaTypeId Id,
                                                         std::vector<std::uint32_t> &Out ) const
{
    const MiddleEnd::TypeSystem::UnitTypes *Source = &Values;
    if ( not Source->Has( Id ) and Services != nullptr and Services->Build != nullptr )
    {
        for ( const UnitView &Unit : Services->Build->Units )
        {
            if ( Unit.Values != nullptr and Unit.Values->Has( Id ) )
            {
                Source = Unit.Values;
                break;
            }
        }
    }

    if ( not Source->Has( Id ) )
    {
        return;
    }

    const MiddleEnd::TypeSystem::SemaType &Value = Source->Get( Id );
    Out.push_back( Value.Base.Value );
    Out.push_back( static_cast<std::uint32_t>( Value.Args.Size() ) );
    for ( const MiddleEnd::TypeSystem::SemaTypeId Arg : Value.Args )
    {
        FlattenValueType( *Source, Arg, Out );
    }
}

Volt::MiddleEnd::TypeSystem::LayoutId
Volt::Backend::Llvm::TypeMapper::LayoutOfValue ( const MiddleEnd::TypeSystem::UnitTypes &Values,
                                                 MiddleEnd::TypeSystem::SemaTypeId Id )
{
    const MiddleEnd::TypeSystem::UnitTypes *Source = &Values;
    if ( not Source->Has( Id ) and Services != nullptr and Services->Build != nullptr )
    {
        for ( const UnitView &Unit : Services->Build->Units )
        {
            if ( Unit.Values != nullptr and Unit.Values->Has( Id ) )
            {
                Source = Unit.Values;
                break;
            }
        }
    }

    if ( not Source->Has( Id ) or Services->Build == nullptr or Services->Build->Types == nullptr )
    {
        return MiddleEnd::TypeSystem::LayoutId{};
    }

    // The head's own arguments are what an instantiation is keyed on; the two
    // header words in front of them belong to the head itself.
    std::vector<std::uint32_t> Flat;
    FlattenValueType( *Source, Id, Flat );
    if ( Flat.size() < 2 )
    {
        return MiddleEnd::TypeSystem::LayoutId{};
    }

    return Services->Instances->Of( *Services->Build->Types, MiddleEnd::TypeSystem::NominalId{ Flat[0] },
                                    std::span<const std::uint32_t>{ Flat }.subspan( 2 ) );
}
