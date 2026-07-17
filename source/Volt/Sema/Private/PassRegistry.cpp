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
        [[nodiscard]] const std::span<const PassInfo> BuildRegistry ()
        {
#define VOLT_PASS( Name, Order ) PassInfo{ #Name, Order, &Name },
            static std::array Registry{
#include "Volt/Sema/PassList.inl"
            };

            static const bool bSorted = []
            {
                std::stable_sort( Registry.begin(), Registry.end(), [] ( const PassInfo &A, const PassInfo &B ) { return A.Order < B.Order; } );
                return true;
            }();
            static_cast<void>( bSorted );

            return std::span<const PassInfo>{ Registry };
        }

    } // namespace

    std::span<const PassInfo> PassRegistry ()
    {
        static const std::span<const PassInfo> Registry = BuildRegistry();
        return Registry;
    }

    std::size_t RunPasses ( PassContext &Context )
    {
        std::size_t Ran = 0;
        for ( const PassInfo &Pass : PassRegistry() )
        {
            Pass.Run( Context );
            ++Ran;
        }
        return Ran;
    }

} // namespace Sema

} // namespace Volt
