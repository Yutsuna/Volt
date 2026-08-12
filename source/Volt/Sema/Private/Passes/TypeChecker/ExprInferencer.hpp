#pragma once

#include "TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

namespace Volt::Sema::TypeCheckerPass
{

SemaTypeId InferExpr ( TypeCheckerContext &Context, Frontend::ExprId Id );

SemaTypeId ComputeExpr ( TypeCheckerContext &Context, Frontend::ExprId Id );

SemaTypeId CallType ( TypeCheckerContext &Context, Frontend::ExprId Id, const Frontend::Call &Expr );

SemaTypeId GenericInstType ( TypeCheckerContext &Context, Frontend::ExprId Id, const Frontend::GenericInst &Expr );

} // namespace Volt::Sema::TypeCheckerPass
