#include "Volt/BackendCore/InitAllSynthesizer.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <cstddef>

bool Volt::Backend::UnitHasInit ( const UnitView &Unit )
{
    return Unit.Ast != nullptr and not Unit.Ast->TopStmts.empty();
}

bool Volt::Backend::UnitHasFini ( const UnitView &Unit )
{
    return Unit.Ast != nullptr and not Unit.Ast->TopTeardown.empty();
}

std::vector<Volt::Backend::InitStep> Volt::Backend::SynthesizeInitAll ( std::span<const UnitView> Units )
{
    std::vector<InitStep> Steps;
    Steps.reserve( Units.size() );
    for ( const UnitView &Unit : Units )
    {
        // Same rule as the teardown below, and for the same reason: a unit whose
        // top level is empty has no `_V_init_<N>` emitted for it, so naming one
        // would call a symbol nothing defines. A stdlib is mostly declarations —
        // most of its units initialise nothing — and calling them was a chain of
        // `ret`s with an exception check between each.
        if ( not UnitHasInit( Unit ) )
        {
            continue;
        }
        Steps.push_back( InitStep{ .Symbol = "_V_init_" + std::to_string( Unit.Ordinal ), .bLast = false } );
    }

    // Set after the filtering, not during it: the last step is the last one that
    // survived, and it is what the check between steps is skipped after.
    if ( not Steps.empty() )
    {
        Steps.back().bLast = true;
    }
    return Steps;
}

std::vector<std::string> Volt::Backend::SynthesizeFiniAll ( std::span<const UnitView> Units )
{
    std::vector<std::string> Steps;
    for ( std::size_t Index = Units.size(); Index > 0; --Index )
    {
        const UnitView &Unit = Units[Index - 1];
        if ( not UnitHasFini( Unit ) )
        {
            continue;
        }
        Steps.push_back( "_V_fini_" + std::to_string( Unit.Ordinal ) );
    }
    return Steps;
}
