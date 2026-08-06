#pragma once

#include "TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

namespace Volt::Sema::TypeCheckerPass
{

// Sweeps the Expr arena, by index, for every expression whose
// `CalleeResolution` names an `EnumCase` member — the same "run only after
// every constraint in the file has had its say" discipline `LiteralLowering`
// uses, and for the identical reason (a construction reached as a call
// argument is inferred before the parameter constrains it).
//
// A no-payload case (`Color::Red`, `TaskStatus::InProgress`) rewrites to a
// plain `IntLiteral` carrying the case's ordinal (`Member::EnumOrdinal`) —
// there is nothing to construct, `self` never exists at runtime for these.
// A payload case (`Optional::Some( x )`) rewrites the enclosing `Call` into
// `tmp = T.new(); tmp.tag = ordinal; tmp.<CaseName> = x; tmp` (a
// `BeginExpr`), mirroring `LowerArrayLit`'s shape exactly. Either way the
// backend never sees an `EnumCase` construction — only core-AST it already
// knows how to emit (`rules/backend-machine-only.md`).
void LowerEnumCases ( TypeCheckerContext &Context );

// Sweeps the Expr arena, by index, for every `CaseExpr` and rewrites each
// `WhenClause` pattern shaped `Call{Callee:Member(target,CaseName),Args:[
// Identifier]}` — CaseLowering's desugar of `.Some(val)` — into a tag
// comparison (`target.tag === ordinal`, the same `===` desugar shape a
// payload-less pattern already gets) plus an implicit binding prepended to
// the clause body: `val = target.<CaseName>`. Every *other* occurrence of
// `val` within the clause's original body (`then val`, `val + 1`, two
// separate uses, ...) is retroactively bound to the same site by a
// recursive `Meta::ForEachField` descent — the same technique
// `ClosureLifting.cpp`'s `RewriteCaptureUses` uses to rewrite every
// occurrence of a captured variable, just matching by *name* against an
// unbound `Identifier` rather than against an existing `ScopeTable`
// binding, since `ScopeResolver` (order 10) ran long before this pattern
// shape existed.
//
// Unlike `LowerEnumCases`, this does **not** skip a deferred (generic-body)
// expression: the case name, its ordinal, and whether it carries a payload
// are per-*nominal* facts (`Member::EnumOrdinal`, `TypeStore::OwnMember`)
// that never depend on a generic argument, so the rewrite can — and must —
// happen once, structurally, on the generic definition's own body; only
// the payload field's *type* is genuinely per-instantiation, and that is
// left to ordinary `Instantiate` substitution exactly as any other member
// access inside a generic body already relies on.
void LowerEnumPatterns ( TypeCheckerContext &Context );

} // namespace Volt::Sema::TypeCheckerPass
