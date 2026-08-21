#include "Volt/BackendCore/InitAllSynthesizer.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <cstddef>

std::vector<Volt::Backend::InitStep> Volt::Backend::SynthesizeInitAll ( std::span<const UnitView> Units )
{
    std::vector<InitStep> Steps;
    Steps.reserve( Units.size() );
    for ( std::size_t Index = 0; Index < Units.size(); ++Index )
    {
        Steps.push_back(
            InitStep{ .Symbol = "_V_init_" + std::to_string( Units[Index].Ordinal ), .bLast = ( Index + 1 == Units.size() ) } );
    }
    return Steps;
}

std::vector<std::string> Volt::Backend::SynthesizeFiniAll ( std::span<const UnitView> Units )
{
    std::vector<std::string> Steps;
    for ( std::size_t Index = Units.size(); Index > 0; --Index )
    {
        const UnitView &Unit = Units[Index - 1];
        if ( Unit.Ast == nullptr or Unit.Ast->TopTeardown.empty() )
        {
            continue;
        }
        Steps.push_back( "_V_fini_" + std::to_string( Unit.Ordinal ) );
    }
    return Steps;
}
