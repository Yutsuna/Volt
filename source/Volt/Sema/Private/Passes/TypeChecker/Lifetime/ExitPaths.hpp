#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"

#include <cstdint>
#include <unordered_set>

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
namespace Volt::Sema::TypeCheckerPass::Lifetime
{

// Does `Body` contain a `Return`/`Break`/`Next` this pass's structural
// recursion cannot reach — i.e. one hiding inside an *expression-position*
// `If`/`CaseExpr`/`BeginExpr` (`x = if c then return 1 else 2 end`)?
//
// A hit currently makes the caller leave the whole method untouched rather
// than risk emitting a partial set of finalize calls. That bail-out is the
// blind spot `.agents/CASCADE_FINALIZE.md` tracks: a method shaped this way
// gets no cleanup at all, silently.
[[nodiscard]] bool ContainsUnstructuredExit ( const Frontend::AstContext &Ast, const Frontend::StmtList &Body );

// Every name that could be the value handed back to the caller on *some*
// path out of the expression `Id` — the move-out exemption's input.
//
// A candidate named here is `Moved`, not `Owned`, at that exit site: freeing
// it would hand the caller a dangling buffer. This walks every branch of an
// `If`/`Ternary`/`CaseExpr`/`BeginExpr` tail rather than only the literal
// syntactic tail, and collects *all* of them — see the implementation for
// the use-after-free that forced it.
void CollectTailIdentifierNames ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out );

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
