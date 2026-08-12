#pragma once

#include "TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

namespace Volt::Sema::TypeCheckerPass
{

// Rewrites the `Lambda`/`Block` at `Id` into `Proc.new( FuncAddr, env )` — a
// `Call`, `env` heap-allocated whenever the literal captures anything — once
// `Id`'s own ClosureType has settled (not deferred, TypeCheckerContext::
// Redirects's own comment: under a generic definition it never settles here
// at all, and this returns without touching Id). The literal's `Params`/
// `Body` are lifted verbatim into a synthesized top-level `Method` Decl
// (`Ast.TopDecls`, the same injection Phase 0's spike proved), recorded in
// `Context.Ctx.Synth` for the backend to declare/define — never in the
// cross-unit TypeStore (`Sema::SynthesizedFunctions`'s own doc comment: a
// data race across `Driver::CompileRefs`'s parallel unit sweep otherwise).
//
// `Context.Redirects == nullptr` (an ordinary unit's own TypeChecker pass):
// mutates Id's slot in place, same as any other Lowering rewrite. Non-null
// (Sema::ReinstantiateBody, re-walking a generic body's shared literal once
// per concrete instantiation): every new node still comes from `Ast.Add()`,
// but Id's own slot — and any captured-variable use site inside the lifted
// body — is left untouched and the substitution recorded in `*Context.
// Redirects` instead, so a second instantiation finds the same unlowered
// literal rather than the first instantiation's already-typed answer.
void LowerClosureLit ( TypeCheckerContext &Context, Frontend::ExprId Id );

// Sweeps the Expr arena, by index, for every `Lambda`/`Block` still standing
// once the whole file's TypeChecker walk has finished — the same "final
// type only" discipline `LowerArrayLits` uses, for the same reason
// (LiteralLowering.hpp).
void LowerClosureLits ( TypeCheckerContext &Context );

// Reads every closure literal's body and records the two ownership facts a
// callable's single declared member — the `FuncType` claimant's bodyless
// `abstract call` — structurally cannot carry: whether invoking it hands back
// an owned value, and which of its parameters it keeps
// (`TypeCheckerContext::OwnedClosureLiterals` / `ClosureParamEscapes`).
//
// Must run *before* any literal is lifted, because both are keyed by the
// literal's own expression id — which stays the slot the parent calls through
// once the literal has become a `Proc.new`. Called by `LowerClosureLits`, and
// separately by `Sema::ReinstantiateBody`, which lifts literals one at a time.
void AnalyzeClosureLiterals ( TypeCheckerContext &Context );

} // namespace Volt::Sema::TypeCheckerPass
