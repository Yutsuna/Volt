#pragma once

// UnusedChecker — order 35. Walks the ScopeTable and reports bindings nothing
// ever reads. Purely diagnostic: it creates no node and rewrites nothing,
// which is what lets it run after `TypeChecker` without breaking the
// structural invariant in rules/core-ast.md.

#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "VoltMiddleEndAnalysis_export.hpp"

namespace Volt::MiddleEnd::Analysis
{

VOLT_MIDDLEEND_ANALYSIS_EXPORT void UnusedChecker ( Core::PassContext &Context );

} // namespace Volt::MiddleEnd::Analysis
