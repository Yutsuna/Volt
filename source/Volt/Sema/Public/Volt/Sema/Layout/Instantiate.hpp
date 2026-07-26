#pragma once

// Instantiate.hpp — on-demand re-typing of a generic body for one concrete
// instantiation.
//
// `UnitTypes`/`UnitCallees` hold exactly one answer per `ExprId`, because
// TypeChecker runs once per unit. A generic body's `ExprId`s are shared by
// every instantiation that ever calls it — `Array<Int32>#push` and
// `Array<String>#push` are the same AST nodes — so neither can hold more
// than one instantiation's answer at a time, and the first, generic-shaped
// TypeChecker pass leaves every expression that touches a type parameter
// `MarkDeferred` rather than guess.
//
// `ReinstantiateBody` is where a monomorphising backend gets an answer
// anyway: it re-runs the type checker's own expression inferencer over one
// member's declared body with `self` and its generics bound to concrete
// arguments instead of the placeholder holes the first pass left, into a
// *fresh* overlay scoped to just this request. This is monomorphisation's
// only semantic step, and it stays here on purpose (rules/core-ast.md: zero
// type inference in a backend) — a backend decides *when* to instantiate,
// never *how* to type what it finds.

#include "Sema_export.hpp"
#include "Volt/Sema/Layout/CalleeMap.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"
#include "Volt/Sema/Scope/ScopeTable.hpp"

#include <cstdint>
#include <span>

namespace Volt::Frontend
{
class AstContext;
}

namespace Volt::Sema
{

// The per-instantiation overlay: the same shapes a concrete unit
// publishes (UnitTypes, UnitCallees), just scoped to one request rather
// than one file.
struct InstantiatedBody
{

    UnitTypes Values;
    UnitCallees Callees;
};

// Re-type `Entry`'s declared body — found via `Entry.Decl` in `Ast`,
// which must be the AstContext of `Entry.Unit` — with `self` and its
// generics bound to `FlatArgs`: the pre-order flattening of
// `CalleeEntry::Bindings` (the owner's own generics first, then the
// method's), the same encoding `BackendCore::InstanceLayouts` and the
// mangler read. `Scopes` is `Entry.Unit`'s own ScopeTable, read only for
// the structural `Use -> declaration` bindings it already resolved —
// lexical scoping does not change under instantiation, only types do.
//
// `Entry` must name a `Method` with a body (`Kind == Method`, not
// `bAbstract`, no `ExternSymbol`); the caller has already made that
// determination, the same one `DeclareMember`/`DefineMember` make for a
// concrete member.
[[nodiscard]] SEMA_EXPORT InstantiatedBody ReinstantiateBody ( const TypeStore &Store,
                                                               const Frontend::AstContext &Ast,
                                                               const ScopeTable &Scopes,
                                                               const Member &Entry,
                                                               NominalId Owner,
                                                               std::span<const std::uint32_t> FlatArgs );

} // namespace Volt::Sema
