// PhaseTimer.cpp — the one definition of the timing table.
//
// It lives in a translation unit rather than in the header on purpose: under
// `default_library=shared` every module is built with
// `gnu_symbol_visibility: hidden`, so a `static` local inside an inline member
// is emitted once *per module*. The table would then exist several times over
// — `volt` enabling its own copy while `libDriver.so` records into a different,
// still-disabled one, and the report comes out empty. One exported definition
// is the fix (rules/shared-lib-exports.md).

#include "Volt/Core/Support/PhaseTimer.hpp"

#include <cmath>
#include <mutex>

namespace
{

struct Table
{

    std::mutex Guard;
    std::vector<Volt::Core::PhaseSample> Samples;
    bool bEnabled = false;
};

[[nodiscard]] Table &Instance ()
{
    static Table Shared;
    return Shared;
}

// Two decimals, no iostream formatting state to save and restore.
[[nodiscard]] std::string Milliseconds ( double Value )
{
    const long Hundredths = std::lround( Value * 100.0 );
    return std::to_string( Hundredths / 100 ) + "." + ( Hundredths % 100 < 10 ? "0" : "" ) + std::to_string( Hundredths % 100 ) +
           " ms";
}

} // namespace

void Volt::Core::PhaseTimings::SetEnabled ( bool bValue )
{
    Instance().bEnabled = bValue;
}

bool Volt::Core::PhaseTimings::Enabled ()
{
    return Instance().bEnabled;
}

void Volt::Core::PhaseTimings::Record ( std::string_view Name, double InMilliseconds )
{
    Table &Shared = Instance();
    if ( not Shared.bEnabled )
    {
        return;
    }

    // The Driver brackets seams from inside its jthread pool, so this is
    // contended — but a seam runs O(1) times per build (it brackets a whole
    // phase, never a node), so the lock can never become the cost it measures.
    const std::lock_guard<std::mutex> Lock( Shared.Guard );
    for ( PhaseSample &Sample : Shared.Samples )
    {
        if ( Sample.Name == Name )
        {
            Sample.Milliseconds += InMilliseconds;
            ++Sample.Count;
            return;
        }
    }
    Shared.Samples.push_back( PhaseSample{ .Name = std::string( Name ), .Milliseconds = InMilliseconds, .Count = 1 } );
}

std::vector<Volt::Core::PhaseSample> Volt::Core::PhaseTimings::Snapshot ()
{
    Table &Shared = Instance();
    const std::lock_guard<std::mutex> Lock( Shared.Guard );
    return Shared.Samples;
}

void Volt::Core::PhaseTimings::Reset ()
{
    Table &Shared = Instance();
    const std::lock_guard<std::mutex> Lock( Shared.Guard );
    Shared.Samples.clear();
}

std::vector<std::string> Volt::Core::FormatPhaseTimings ()
{
    const std::vector<PhaseSample> Samples = PhaseTimings::Snapshot();

    std::size_t Width = 0;
    double Total      = 0.0;
    for ( const PhaseSample &Sample : Samples )
    {
        Width = Sample.Name.size() > Width ? Sample.Name.size() : Width;
        Total += Sample.Milliseconds;
    }

    std::vector<std::string> Lines;
    Lines.reserve( Samples.size() + 1 );
    for ( const PhaseSample &Sample : Samples )
    {
        Lines.push_back( "  " + Sample.Name + std::string( Width - Sample.Name.size(), ' ' ) + " = " +
                         Milliseconds( Sample.Milliseconds ) );
    }
    if ( not Samples.empty() )
    {
        Lines.push_back( "  " + std::string( Width, '-' ) + "   " + Milliseconds( Total ) + " total" );
    }
    return Lines;
}
