// Compile-time + smoke check for MiddleEnd::Core: pass registry ordering.

#ifndef DEBUG_NO_STATIC_ASSERT

    #include "Volt/MiddleEnd/Core/Pass.hpp"

    #include <cstddef>
    #include <span>

namespace Volt
{

namespace MiddleEnd
{

namespace Core
{

    namespace
    {

        [[maybe_unused]] bool RunCoreSmokeTest ()
        {
            const std::span<const PassInfo> Registry = PassRegistry();
            if ( Registry.empty() )
            {
                return false;
            }
            for ( std::size_t Idx = 1; Idx < Registry.size(); ++Idx )
            {
                if ( Registry[Idx - 1].Order > Registry[Idx].Order )
                {
                    return false;
                }
            }
            return true;
        }

    } // namespace

} // namespace Core

} // namespace MiddleEnd

} // namespace Volt

#endif // DEBUG_NO_STATIC_ASSERT
