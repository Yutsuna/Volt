// LayoutOfValue.cpp — the SemaTypeId side of the MonoRequest currency.
//
// See InstanceLayout.hpp. An inferred type is per unit and per expression; a
// memory shape is build-wide. Flattening is what bridges them, and it is the
// same encoding InstanceLayouts, Monomorphizer and Mangler already read — so a
// layout, a symbol and a queue entry can never mean different instantiations.
//
// Nothing here is target-specific, which is why it lives beside InstanceLayout
// rather than beside any one emitter's type mapper.

#include "Volt/BackendCore/InstanceLayout.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace
{

// A monomorphised body reads types belonging to the unit that *declared* the
// generic, not to the one instantiating it, so the unit handed in is only the
// first place to look.
[[nodiscard]] const Volt::MiddleEnd::TypeSystem::UnitTypes *SourceOf ( const Volt::Backend::BackendInput &Build,
                                                                       const Volt::MiddleEnd::TypeSystem::UnitTypes &Values,
                                                                       Volt::MiddleEnd::TypeSystem::SemaTypeId Id )
{
    if ( Values.Has( Id ) )
    {
        return &Values;
    }
    for ( const Volt::Backend::UnitView &Unit : Build.Units )
    {
        if ( Unit.Values != nullptr and Unit.Values->Has( Id ) )
        {
            return Unit.Values;
        }
    }
    return &Values;
}

} // namespace

void Volt::Backend::FlattenValueType ( const BackendInput &Build,
                                       const MiddleEnd::TypeSystem::UnitTypes &Values,
                                       MiddleEnd::TypeSystem::SemaTypeId Id,
                                       std::vector<std::uint32_t> &Out )
{
    const MiddleEnd::TypeSystem::UnitTypes *Source = SourceOf( Build, Values, Id );
    if ( not Source->Has( Id ) )
    {
        return;
    }

    const MiddleEnd::TypeSystem::SemaType &Value = Source->Get( Id );
    Out.push_back( Value.Base.Value );
    Out.push_back( static_cast<std::uint32_t>( Value.Args.Size() ) );
    for ( const MiddleEnd::TypeSystem::SemaTypeId Arg : Value.Args )
    {
        FlattenValueType( Build, *Source, Arg, Out );
    }
}

Volt::MiddleEnd::TypeSystem::LayoutId Volt::Backend::LayoutOfValue ( const BackendInput &Build,
                                                                     InstanceLayouts &Instances,
                                                                     const MiddleEnd::TypeSystem::UnitTypes &Values,
                                                                     MiddleEnd::TypeSystem::SemaTypeId Id )
{
    const MiddleEnd::TypeSystem::UnitTypes *Source = SourceOf( Build, Values, Id );
    if ( not Source->Has( Id ) or Build.Types == nullptr )
    {
        return MiddleEnd::TypeSystem::LayoutId{};
    }

    // The head's own arguments are what an instantiation is keyed on; the two
    // header words in front of them belong to the head itself.
    std::vector<std::uint32_t> Flat;
    FlattenValueType( Build, *Source, Id, Flat );
    if ( Flat.size() < 2 )
    {
        return MiddleEnd::TypeSystem::LayoutId{};
    }

    return Instances.Of( *Build.Types, MiddleEnd::TypeSystem::NominalId{ Flat[0] },
                         std::span<const std::uint32_t>{ Flat }.subspan( 2 ) );
}
