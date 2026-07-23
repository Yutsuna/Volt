#pragma once

#include "TypeCheckerContext.hpp"

#include <string_view>

namespace Volt::Sema::TypeCheckerPass
{

constexpr std::string_view IndexOperator = "[]";

[[nodiscard]] Resolution LookupOn ( TypeCheckerContext &Context, SemaTypeId Receiver, std::string_view Name );

void CheckMemberSelf ( TypeCheckerContext &Context, Core::SourceRange Loc, const Resolution &Found, bool bReceiverIsNakedType );

void CheckDotCallSelf ( TypeCheckerContext &Context, Core::SourceRange Loc, const Resolution &Found );

[[nodiscard]] bool IsBuiltinPrimitiveOp ( std::string_view Name );

[[nodiscard]] SemaTypeId MemberType (
    TypeCheckerContext &Context, Core::SourceRange Loc, SemaTypeId Receiver, bool bReceiverIsNakedType, std::string_view Name );

void CheckCallArgs ( TypeCheckerContext &Context,
                     Core::SourceRange Loc,
                     const Resolution &Found,
                     const Frontend::ExprList &Args );

void CheckArity ( TypeCheckerContext &Context, Core::SourceRange Loc, NominalId Base, std::size_t Given );

} // namespace Volt::Sema::TypeCheckerPass
