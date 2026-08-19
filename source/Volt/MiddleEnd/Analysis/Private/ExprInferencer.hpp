#pragma once

#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"

namespace Volt::MiddleEnd::Analysis
{

SemaTypeId InferExpr ( TypeCheckerContext &Context, Frontend::ExprId Id );

SemaTypeId ComputeExpr ( TypeCheckerContext &Context, Frontend::ExprId Id );

SemaTypeId CallType ( TypeCheckerContext &Context, Frontend::ExprId Id, const Frontend::Call &Expr );

SemaTypeId GenericInstType ( TypeCheckerContext &Context, Frontend::ExprId Id, const Frontend::GenericInst &Expr );

Frontend::ExprList
FillDefaultArguments ( TypeCheckerContext &Context, const Resolution &Found, const Frontend::ExprList &GivenArgs );

void LowerParenlessCalls ( TypeCheckerContext &Context );

} // namespace Volt::MiddleEnd::Analysis
