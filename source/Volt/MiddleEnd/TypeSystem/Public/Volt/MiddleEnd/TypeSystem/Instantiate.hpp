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

#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/IR/CalleeMap.hpp"
#include "Volt/MiddleEnd/IR/ExprRedirect.hpp"
#include "Volt/MiddleEnd/IR/SynthesizedFunctions.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndTypeSystem_export.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>

namespace Volt::Frontend
{
class AstContext;
}

// Forward declaration only, deliberately not an include: TypeSystem must not
// depend on Resolver (Resolver depends on TypeSystem — REFACTO.md's DAG).
// ReinstantiateBody only ever holds this by reference and forwards it on, so
// an incomplete type is enough here; the complete definition reaches
// Reinstantiate.cpp transitively through the (still Analysis-tier, not yet
// migrated) TypeCheckerContext.hpp it already includes.
namespace Volt::MiddleEnd::Resolver
{
class ScopeTable;
} // namespace Volt::MiddleEnd::Resolver

namespace Volt::MiddleEnd::TypeSystem
{

using IR::ExprRedirectMap;
using IR::SynthesizedFunctions;
using IR::UnitCallees;
using Resolver::ScopeTable;

// The per-instantiation overlay: the same shapes a concrete unit
// publishes (UnitTypes, UnitCallees), just scoped to one request rather
// than one file.
struct InstantiatedBody
{

    UnitTypes Values;
    UnitCallees Callees;
    SynthesizedFunctions Synth;
    ExprRedirectMap Redirects;
    SemaTypeId ReturnType{};
    bool bFullInstantiation = false;
};

VOLT_MIDDLEEND_TYPESYSTEM_EXPORT void
FlattenValueType ( const UnitTypes &Values, SemaTypeId Id, std::vector<std::uint32_t> &Out );

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
// concrete member. Any Lambda/Block literal reached while re-typing that
// body is lowered fresh into `Result.Synth`/`Result.Redirects` — see
// ExprRedirectMap's own comment; the declaring unit's own (pre-instantiation)
// SynthesizedFunctions table is never consulted, since a literal touching a
// type parameter has nothing concrete recorded there to reuse.
[[nodiscard]] VOLT_MIDDLEEND_TYPESYSTEM_EXPORT InstantiatedBody ReinstantiateBody ( const TypeStore &Store,
                                                                                    const Frontend::AstContext &Ast,
                                                                                    const ScopeTable &Scopes,
                                                                                    const Member &Entry,
                                                                                    NominalId Owner,
                                                                                    std::span<const std::uint32_t> FlatArgs );

[[nodiscard]] VOLT_MIDDLEEND_TYPESYSTEM_EXPORT SemaTypeId InferMethodReturnType ( const TypeStore &Store,
                                                                                  const Frontend::AstContext &Ast,
                                                                                  const ScopeTable &Scopes,
                                                                                  const Member &Entry,
                                                                                  NominalId Owner,
                                                                                  std::span<const std::uint32_t> FlatArgs,
                                                                                  UnitTypes &Values );

[[nodiscard]] inline SemaTypeId InferMethodReturnType ( const TypeStore &Store,
                                                        const Frontend::AstContext &Ast,
                                                        const ScopeTable &Scopes,
                                                        const Member &Entry,
                                                        NominalId Owner,
                                                        std::span<const std::uint32_t> FlatArgs )
{
    UnitTypes LocalValues;
    return InferMethodReturnType( Store, Ast, Scopes, Entry, Owner, FlatArgs, LocalValues );
}

} // namespace Volt::MiddleEnd::TypeSystem
