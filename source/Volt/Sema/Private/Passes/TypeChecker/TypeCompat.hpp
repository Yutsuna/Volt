#pragma once

#include "TypeCheckerContext.hpp"

#include <cstdint>

namespace Volt::Sema::TypeCheckerPass
{

// **The** assignability predicate — is a `Value` of one type acceptable where
// a `Target` of another is expected?
//
// One predicate, deliberately. Argument passing, local initialisation,
// assignment, `return`, a method's trailing expression and a parameter default
// are six spellings of the same question; two predicates would eventually
// answer it differently and one of the six would go quiet. Same lesson as
// `IsBuiltinOpOn`.
//
// Call it **after** `ConstrainExprType`, never before: constraint propagation
// is what narrows an unconstrained literal to the expected type, so
// `a : UInt64 = 8` is only assignable once the `8` has been narrowed.
//
// An invalid `Target` or `Value` means "not resolved" and is *accepted* — the
// diagnostic for a type that never resolved belongs to whoever failed to
// resolve it, and reporting it again here would double every such error.
[[nodiscard]] bool IsAssignable ( const TypeCheckerContext &Context, SemaTypeId Target, SemaTypeId Value );

// Which of the five value positions is being checked. Only the wording of the
// diagnostic depends on it — the predicate above does not — so a sixth site is
// one enumerator and one line in the message, never a second rule.
enum class EAssignSite : std::uint8_t
{

    LocalDecl,
    Assign,
    Return,
    Trailing,
    ParamDefault,
};

// Report unless `ValueExpr`'s inferred type fits `Target`. Silent when either
// side never resolved, and silent when the value is `NoReturn` — `raise`
// produces no value, so it fits every slot.
//
// Reads the type already recorded for `ValueExpr` rather than inferring it
// again: by the time any of the five sites calls this, `ConstrainExprType` has
// run and the literal has been narrowed. Calling it earlier would reject
// `a : UInt64 = 8`.
void CheckAssignable ( TypeCheckerContext &Context, Frontend::ExprId ValueExpr, SemaTypeId Target, EAssignSite Site );

} // namespace Volt::Sema::TypeCheckerPass
