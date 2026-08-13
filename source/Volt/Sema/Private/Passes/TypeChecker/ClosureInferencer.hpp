#pragma once

#include "Sema_export.hpp"
#include "TypeCheckerContext.hpp"

#include <string_view>

namespace Volt::Sema::TypeCheckerPass
{

[[nodiscard]] Core::SmallVec<SemaTypeId, 2> BindClosureParams ( TypeCheckerContext &Context, const Frontend::ParamList &Params );

// Body is taken by value, not by reference: a caller may hold it as a field
// of a live arena reference (a RescueClause's own Body, read via
// Context.Ctx.Ast.Stmt(ClauseId)), and WalkStmt below can Add() onto that
// same Stmt arena mid-loop since LowerArrayLit started rewriting AST nodes
// during TypeChecker — rules/ast-rewrite.md.
[[nodiscard]] SEMA_EXPORT SemaTypeId TrailingType ( TypeCheckerContext &Context, Frontend::StmtList Body );

[[nodiscard]] SemaTypeId ClosureType ( TypeCheckerContext &Context,
                                       std::string_view NodeKind,
                                       SemaTypeId Result,
                                       const Core::SmallVec<SemaTypeId, 2> &Params );

} // namespace Volt::Sema::TypeCheckerPass
