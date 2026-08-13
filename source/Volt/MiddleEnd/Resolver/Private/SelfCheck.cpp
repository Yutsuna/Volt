#include "Volt/MiddleEnd/Resolver/ClosureFrame.hpp"
#include "Volt/MiddleEnd/Resolver/InterfaceRegistry.hpp"
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"
#include "Volt/MiddleEnd/Resolver/TypeBinder.hpp"

namespace Volt::MiddleEnd::Resolver
{

static_assert( sizeof( ScopeId ) > 0, "Resolver self-check" );

} // namespace Volt::MiddleEnd::Resolver
