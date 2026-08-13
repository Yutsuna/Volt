#pragma once

// AstInvariant — order 40, the contract a backend is handed, checked on every
// build (rules/core-ast.md): no `VOLT_EXPR_SUGAR` node survives `Lowering`,
// and every expression in value position has a type. Reads the sugar set
// straight out of `Nodes.inl`, so a node marked sugar and never lowered is a
// build failure rather than something a backend author discovers.

#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "VoltMiddleEndAnalysis_export.hpp"

namespace Volt::MiddleEnd::Analysis
{

VOLT_MIDDLEEND_ANALYSIS_EXPORT void AstInvariant ( Core::PassContext &Context );

} // namespace Volt::MiddleEnd::Analysis
