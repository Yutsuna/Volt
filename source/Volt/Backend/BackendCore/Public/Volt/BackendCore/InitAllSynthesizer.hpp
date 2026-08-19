#pragma once

// InitAllSynthesizer.hpp — the body of `_V_init_all`, expressed as a
// backend-neutral sequence of steps rather than hand-rolled IR.
//
// Every backend reads the same steps and emits its own code — the
// decision of *what* to call and *when* to stop is shared, only the
// instruction encoding differs. Exactly the same split VTableEngine
// already uses for vtable layouts.

#include "BackendCore_export.hpp"
#include "Volt/BackendCore/BackendInput.hpp"

#include <span>
#include <string>
#include <vector>

namespace Volt::Backend
{

// One step of the `_V_init_all` body: call a unit initializer,
// then — unless it is the last — check the exception tag and
// bail out if an exception is in flight.
struct InitStep
{
    std::string Symbol; // "_V_init_0", "_V_init_1", ...
    bool bLast = false; // true -> no post-call check needed
};

// Build the flat sequence from the circuit's unit views.
[[nodiscard]] BACKENDCORE_EXPORT std::vector<InitStep> SynthesizeInitAll ( std::span<const UnitView> Units );

} // namespace Volt::Backend
