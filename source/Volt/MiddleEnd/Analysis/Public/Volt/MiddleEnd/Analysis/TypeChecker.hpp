#pragma once

// TypeChecker — order 30, the pass that gives every expression a type and
// resolves the members types make available. `PassList.inl` names it and
// `MiddleEnd/Core/Pass.hpp` forward-declares it, so the registry reaches it
// with no include at all; this header exists for the direct callers (tools,
// tests) that want the entry point without the manifest.
//
// It is also where five post-walk sweeps run — `Lowering::LowerArrayLits`,
// `LowerHashLits`, `LowerStringLits`, `LowerEnumPatterns`/`LowerEnumCases`,
// `LowerClosureLits` and `InsertFinalizeCalls` — each of which needs a type
// that has finished settling rather than one observed mid-walk. See
// rules/core-ast.md and `Volt/MiddleEnd/Lowering/LoweringPasses.hpp`.

#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "VoltMiddleEndAnalysis_export.hpp"

namespace Volt::MiddleEnd::Analysis
{

VOLT_MIDDLEEND_ANALYSIS_EXPORT void TypeChecker ( Core::PassContext &Context );

} // namespace Volt::MiddleEnd::Analysis
