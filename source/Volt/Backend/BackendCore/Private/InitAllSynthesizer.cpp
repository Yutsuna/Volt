#include "Volt/BackendCore/InitAllSynthesizer.hpp"

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
