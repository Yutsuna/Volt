#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndOptimisations_export.hpp"

#include <cstdint>
#include <span>

namespace Volt::MiddleEnd::Optimisations
{

using TypeSystem::EInlineVerdict;

/// Structural summary of a method body used to derive inlining decisions.
struct InlineSummary
{
    std::uint32_t NodeCount      = 0;     // Total Expr and Stmt count
    std::uint32_t CallSites      = 0;     // Number of call expressions
    std::uint32_t BlockCalls     = 0;     // Calls targeting the &block parameter
    bool bHasLoop                = false; // Contains while/loop
    bool bHasEarlyReturn         = false; // Return statement not in final position
    bool bHasRaise               = false; // Contains raise or rescue
    bool bSelfRecursive          = false; // Directly calls itself
    bool bTouchesNonPublicMember = false; // Accesses private or protected members
    bool bHasBlockParam          = false; // Has a &block parameter
};

/// Computes the structural summary of a Method node.
[[nodiscard]] VOLT_MIDDLEEND_OPTIMISATIONS_EXPORT InlineSummary SummarizeMethod ( const Frontend::AstContext &Ast,
                                                                                  const Frontend::Method &MethodNode,
                                                                                  const TypeSystem::TypeStore &Types,
                                                                                  TypeSystem::NominalId Owner = {} );

/// Derives the inlining verdict from an InlineSummary.
[[nodiscard]] VOLT_MIDDLEEND_OPTIMISATIONS_EXPORT EInlineVerdict VerdictOf ( const InlineSummary &Summary,
                                                                             bool bAbstract = false,
                                                                             bool bExternal = false );

/// Serial seam entry point: analyzes all declared methods across all units and
/// stamps Member::InlineVerdict on every Member in Store.
VOLT_MIDDLEEND_OPTIMISATIONS_EXPORT void AnalyzeInlineCandidates ( std::span<const Frontend::AstContext *const> Units,
                                                                   TypeSystem::TypeStore &Store );

} // namespace Volt::MiddleEnd::Optimisations
