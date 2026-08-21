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

// The mirror, for `_V_fini_all`: every unit's teardown, in reverse.
//
// Reverse because destruction is the reverse of construction — a unit
// initialised after another may hold something the earlier one owns — and
// unconditional because teardown runs *because* the program is over, including
// when it is over on account of an exception. There is no early bail-out and
// therefore no `bLast`.
//
// Only units that have something to tear down appear: `_V_fini_<N>` is emitted
// for a unit with a module variable to release and for no other, so naming one
// that does not exist would leave `_V_fini_all` calling an undefined symbol.
[[nodiscard]] BACKENDCORE_EXPORT std::vector<std::string> SynthesizeFiniAll ( std::span<const UnitView> Units );

} // namespace Volt::Backend
