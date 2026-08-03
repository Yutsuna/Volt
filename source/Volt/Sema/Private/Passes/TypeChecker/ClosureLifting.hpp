#pragma once

#include "TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

namespace Volt::Sema::TypeCheckerPass
{

// Rewrites the `Lambda`/`Block` at `Id` in place into `Proc.new( FuncAddr,
// nil )` — a `Call` — once `Id`'s own ClosureType has settled. The literal's
// `Params`/`Body` are lifted verbatim into a synthesized top-level `Method`
// Decl (`Ast.TopDecls`, the same injection Phase 0's spike proved), recorded
// in `Context.Ctx.Synth` for the backend to declare/define — never in the
// cross-unit TypeStore (`Sema::SynthesizedFunctions`'s own doc comment: a
// data race across `Driver::CompileRefs`'s parallel unit sweep otherwise).
//
// 3a scope only: a closure whose `ScopeTable::CapturesOf` is non-empty is
// left untouched — its `env` would need the `Pointer<UInt8>`-arithmetic
// rewrite `.agents/PLAN_CLOSURE_LOWERING.md`'s Phase 3b still owns. Until 3b
// lands, `Lambda`/`Block` cannot move to `VOLT_EXPR_SUGAR`: a capturing
// closure survives this sweep on purpose.
void LowerClosureLit ( TypeCheckerContext &Context, Frontend::ExprId Id );

// Sweeps the Expr arena, by index, for every `Lambda`/`Block` still standing
// once the whole file's TypeChecker walk has finished — the same "final
// type only" discipline `LowerArrayLits` uses, for the same reason
// (LiteralLowering.hpp).
void LowerClosureLits ( TypeCheckerContext &Context );

} // namespace Volt::Sema::TypeCheckerPass
