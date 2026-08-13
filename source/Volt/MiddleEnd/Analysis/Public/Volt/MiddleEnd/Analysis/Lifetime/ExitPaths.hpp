#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/Analysis/AnalysisTypes.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

// How control leaves a region.
//
// Volt's RAII model rests on one rule: *every path that leaves a lifetime
// region must traverse that region's cleanup boundary*. This file owns the
// analysis half of that rule — finding the exits and deciding what each one
// hands back — while `CleanupRegion` owns the emission half. Neither knows
// how the other is represented.
//
// Nothing here needs a `TypeCheckerContext`: an exit is a purely syntactic
// property of a body, so these are plain functions over an `AstContext`.
namespace Volt::MiddleEnd::Analysis::Lifetime
{

// Every `If`/`CaseExpr`/`BeginExpr` occurrence reachable from the statement
// `Id` through *expression* fields — that is, every nested `StmtList` the
// statement owns, wherever it sits.
//
// This is what makes an exit in expression position ordinary rather than
// special. `x = if c then return 1 else 2 end` puts a `StmtList` under an
// `Assign`'s value; `f( begin ... end )` puts one under a `Call` argument.
// Statement position is not a distinct case, only the shallowest one: an
// `ExprStmt` whose expression *is* the `If` is found by the same walk, at
// depth zero.
//
// Only the **outermost** construct of any subtree is reported. Descending
// further would be double work and, worse, double instrumentation: the
// caller recurses into the branch bodies it is handed, and each statement
// there is put through this same walk.
//
// Encounter order, and by value — the caller rewrites arena slots as it goes
// (rules/ast-rewrite.md), so nothing may be held as a reference across it.
[[nodiscard]] std::vector<Frontend::ExprId> CollectNestedBlockExprs ( const Frontend::AstContext &Ast, Frontend::StmtId Id );

// Every name that could be the value handed back to the caller on *some*
// path out of the expression `Id` — the move-out exemption's input.
//
// A candidate named here is `Moved`, not `Owned`, at that exit site: freeing
// it would hand the caller a dangling buffer. This walks every branch of an
// `If`/`Ternary`/`CaseExpr`/`BeginExpr` tail rather than only the literal
// syntactic tail, and collects *all* of them — see the implementation for
// the use-after-free that forced it.
void CollectTailIdentifierNames ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out );

} // namespace Volt::MiddleEnd::Analysis::Lifetime
