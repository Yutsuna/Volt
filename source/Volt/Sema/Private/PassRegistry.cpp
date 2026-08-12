// PassRegistry.cpp — turns the PassList.inl manifest into a sorted, runnable
// registry. The manifest is the single source of truth: this file contains no
// per-pass knowledge beyond wiring the generated table.

#include "Volt/Sema/Pass.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace Volt
{

namespace Sema
{

    namespace
    {

        // Build the registry once from the manifest, then sort by Order so
        // the Driver can run passes deterministically regardless of the
        // order the entries happen to appear in PassList.inl.
        [[nodiscard]] std::span<const PassInfo> BuildRegistry ()
        {
#define VOLT_PASS( Name, Order, Kind ) PassInfo{ #Name, Order, EPassKind::Kind, &Name },
            static std::array Registry{
#include "Volt/Sema/PassList.inl"
            };

            static const bool bSorted = []
            {
                std::ranges::stable_sort( Registry, [] ( const PassInfo &A, const PassInfo &B ) { return A.Order < B.Order; } );
                return true;
            }();
            static_cast<void>( bSorted );

            return std::span<const PassInfo>{ Registry };
        }

    } // namespace

} // namespace Sema

} // namespace Volt

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::span<const Volt::Sema::PassInfo> Volt::Sema::PassRegistry ()
{
    static const std::span<const PassInfo> Registry = BuildRegistry();
    return Registry;
}

std::size_t Volt::Sema::RunPasses ( PassContext &Context )
{
    std::size_t Ran = 0;
    for ( const PassInfo &Pass : PassRegistry() )
    {
        Pass.Run( Context );
        ++Ran;
    }
    return Ran;
}

std::size_t Volt::Sema::RunPasses ( PassContext &Context, EPassKind Only )
{
    std::size_t Ran = 0;
    for ( const PassInfo &Pass : PassRegistry() )
    {
        if ( Pass.Kind == Only )
        {
            Pass.Run( Context );
            ++Ran;
        }
    }
    return Ran;
}

std::size_t Volt::Sema::RunPasses ( PassContext &Context, const int OrderBegin, const int OrderEnd )
{
    std::size_t Ran = 0;
    for ( const PassInfo &Pass : PassRegistry() )
    {
        if ( Pass.Order >= OrderBegin and Pass.Order < OrderEnd )
        {
            Pass.Run( Context );
            ++Ran;
        }
    }
    return Ran;
}

int Volt::Sema::LoweredSeamOrder ()
{
    static const int Seam = []
    {
        int Last = 0;
        for ( const PassInfo &Pass : PassRegistry() )
        {
            if ( Pass.Kind == EPassKind::Lowering )
            {
                Last = std::max( Last, Pass.Order );
            }
        }
        return Last + 1;
    }();
    return Seam;
}
