#pragma once

#include "TypeCheckerContext.hpp"

#include <string_view>

namespace Volt::Sema::TypeCheckerPass
{

[[nodiscard]] Core::SmallVec<SemaTypeId, 2> BindClosureParams ( TypeCheckerContext &Context, const Frontend::ParamList &Params );

[[nodiscard]] SemaTypeId TrailingType ( TypeCheckerContext &Context, const Frontend::StmtList &Body );

[[nodiscard]] SemaTypeId ClosureType ( TypeCheckerContext &Context,
                                       std::string_view NodeKind,
                                       SemaTypeId Result,
                                       const Core::SmallVec<SemaTypeId, 2> &Params );

} // namespace Volt::Sema::TypeCheckerPass
