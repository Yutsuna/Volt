#pragma once

#include "TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

namespace Volt::Sema::TypeCheckerPass
{

// Rewrites the `ArrayLit` at `Id` in place into `tmp = T.new(); tmp << e0;
// tmp << e1; ...; tmp` (a BeginExpr), using `LiteralType` as its final,
// already-settled SemaTypeId. `Elements` is a caller-owned copy
// (rules/ast-rewrite.md). `<<` is resolved through the ordinary
// operator-resolution path (MemberType, via a synthesized Binary), so a type
// claiming ArrayLit but declaring no `<<` fails with the same diagnostic an
// unresolved method call always gets — see .agents/PLAN_LITERAL_LOWERING.md.
void LowerArrayLit ( TypeCheckerContext &Context, Frontend::ExprId Id, Frontend::ExprList Elements, SemaTypeId LiteralType );

// Sweeps the Expr arena, by index, for every `ArrayLit` still standing once
// the whole file's TypeChecker walk has finished — never mid-walk. A literal
// passed as a call argument is InferExpr'd (naturally, bottom-up) *before*
// CheckCallArgs gets to push the parameter's type down via ConstrainExprType
// (CallType: "arguments are bound before being checked"); rewriting inline,
// the moment either path first settles a type, permanently bakes in
// whichever one ran first — wrong the moment a later ConstrainExprType call
// was going to narrow it further. Run only after every constraint in the
// file has had its say, this reads each literal's *final* SemaTypeId
// (Values.ExprType), the same census discipline rules/ast-rewrite.md uses
// for every other arena sweep (see RejectNilableTypes, TypeChecker.cpp).
void LowerArrayLits ( TypeCheckerContext &Context );

} // namespace Volt::Sema::TypeCheckerPass
