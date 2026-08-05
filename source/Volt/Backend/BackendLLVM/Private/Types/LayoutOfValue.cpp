// LayoutOfValue.cpp — the SemaTypeId half of TypeMapper.
//
// An *inferred* type (Sema::SemaTypeId, per unit and per expression) becomes a
// *memory shape* (Sema::LayoutId, build-wide) by being flattened into the
// MonoRequest encoding first — the one currency InstanceLayouts, Monomorphizer
// and Mangler share, so a layout, a symbol and a queue entry can never mean
// different instantiations. Split from TypeMapper.cpp because it is that
// currency's own concern and reads no llvm:: type at all.

#include "Types/TypeMapper.hpp"

#include <cstdint>
#include <span>
#include <vector>

void Volt::Backend::Llvm::TypeMapper::FlattenValueType ( const Sema::UnitTypes &Values,
                                                         Sema::SemaTypeId Id,
                                                         std::vector<std::uint32_t> &Out ) const
{
    if ( not Values.Has( Id ) )
    {
        return;
    }

    const Sema::SemaType &Value = Values.Get( Id );
    Out.push_back( Value.Base.Value );
    Out.push_back( static_cast<std::uint32_t>( Value.Args.Size() ) );
    for ( const Sema::SemaTypeId Arg : Value.Args )
    {
        FlattenValueType( Values, Arg, Out );
    }
}

Volt::Sema::LayoutId Volt::Backend::Llvm::TypeMapper::LayoutOfValue ( const Sema::UnitTypes &Values, Sema::SemaTypeId Id )
{
    if ( not Values.Has( Id ) or Services->Build == nullptr or Services->Build->Types == nullptr )
    {
        return Sema::LayoutId{};
    }

    // The head's own arguments are what an instantiation is keyed on; the two
    // header words in front of them belong to the head itself.
    std::vector<std::uint32_t> Flat;
    FlattenValueType( Values, Id, Flat );
    if ( Flat.size() < 2 )
    {
        return Sema::LayoutId{};
    }

    return Services->Instances->Of( *Services->Build->Types, Sema::NominalId{ Flat[0] },
                                    std::span<const std::uint32_t>{ Flat }.subspan( 2 ) );
}
