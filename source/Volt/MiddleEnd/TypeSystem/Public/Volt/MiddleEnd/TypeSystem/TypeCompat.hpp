#pragma once

#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "VoltMiddleEndTypeSystem_export.hpp"

#include <cstdint>

namespace Volt::Sema::TypeCheckerPass
{
struct TypeCheckerContext;
}

namespace Volt::MiddleEnd::TypeSystem
{

using TypeCheckerContext = Volt::Sema::TypeCheckerPass::TypeCheckerContext;

// **The** assignability predicate — is a `Value` of one type acceptable where
// a `Target` of another is expected?
[[nodiscard]] VOLT_MIDDLEEND_TYPESYSTEM_EXPORT bool
IsAssignable ( const TypeCheckerContext &Context, SemaTypeId Target, SemaTypeId Value );

enum class EAssignSite : std::uint8_t
{
    LocalDecl,
    Assign,
    Return,
    Trailing,
    ParamDefault,
};

VOLT_MIDDLEEND_TYPESYSTEM_EXPORT void
CheckAssignable ( TypeCheckerContext &Context, Frontend::ExprId ValueExpr, SemaTypeId Target, EAssignSite Site );

} // namespace Volt::MiddleEnd::TypeSystem
