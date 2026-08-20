#pragma once

#include "VoltMiddleEndOptimisations_export.hpp"

namespace Volt::MiddleEnd::Analysis
{
struct TypeCheckerContext;
}

namespace Volt::MiddleEnd::Optimisations
{

/// Post-walk optimization pass that inlines calls receiving a non-escaping block/lambda argument.
/// Runs inside TypeChecker before LowerClosureLits.
VOLT_MIDDLEEND_OPTIMISATIONS_EXPORT void InlineBlockCalls ( Analysis::TypeCheckerContext &State );

} // namespace Volt::MiddleEnd::Optimisations
