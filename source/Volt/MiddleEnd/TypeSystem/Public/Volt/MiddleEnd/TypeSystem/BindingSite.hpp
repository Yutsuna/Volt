#pragma once

// BindingSite.hpp — where a name was declared, as a typed AST Id.
//
// Extracted out of Resolver's ScopeTable.hpp for the same reason
// IR/ExprRedirect.hpp was extracted out of Instantiate.hpp: TypeSystem needs
// this type (UnitTypes::SiteTypes, SemaType.hpp) but must not depend on
// Resolver — Resolver depends on TypeSystem, never the other way (see
// REFACTO.md's DAG). BindingSite itself needs nothing but Frontend AST Ids,
// so it belongs at the lower layer both modules can see; Resolver::ScopeTable
// still owns the full scoping behaviour built on top of it.

#include "Volt/Frontend/AST/Node.hpp"

#include <cstddef>
#include <functional>
#include <variant>

namespace Volt::MiddleEnd::TypeSystem
{

// The ExprId arm is the *implicit* local — `buf = expr` with no `: Type`.
// The parser cannot tell that form from a reassignment (it has no scope
// information), so it builds an Assign over a bare Identifier and never a
// LocalDecl; the declaration is therefore the Assign's Target expression
// itself, which is the only Id that names this binding uniquely (one
// statement may contain several, nested inside call arguments).
using BindingSite = std::variant<Frontend::StmtId,  // LocalDecl
                                 Frontend::ParamId, // Method/Block parameter
                                 Frontend::DeclId,  // Field / member decl
                                 Frontend::ExprId>; // implicit local (Assign target)

// Does this site name *storage in a frame* — a parameter, a declared
// local or an implicit one — as opposed to a member declaration, which
// is reached through a receiver and only ever recorded here as tooling
// metadata? Capture analysis and implicit-local declaration both turn on
// exactly this distinction, so they share one predicate.
[[nodiscard]] inline bool IsValueBinding ( const BindingSite &Site )
{
    return not std::holds_alternative<Frontend::DeclId>( Site );
}

// Structural key hash for maps keyed by BindingSite (TypeChecker's local
// types): alternative index + the TypedId value inside it.
struct BindingSiteHash
{

    [[nodiscard]] std::size_t operator()( const BindingSite &Site ) const noexcept
    {
        const auto Value = std::visit( [] ( const auto &Id ) { return Id.Value; }, Site );
        return std::hash<std::size_t>{}( ( Site.index() << 32U ) ^ Value );
    }
};

} // namespace Volt::MiddleEnd::TypeSystem
