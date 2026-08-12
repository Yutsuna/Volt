#pragma once

#include "Sema_export.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <span>

// Does calling this member hand back something the caller owns?
//
// The question RAII cannot avoid and cannot guess. `String#+` returns a
// freshly allocated buffer the caller must release; `Array<T>#[]` returns a
// view onto an element the receiver still owns. Both are "a `Call` whose
// result type declares `finalize`", so classifying a temporary on that shape
// alone is a double free waiting for the second case — which is why
// `Member::bReturnsOwned` exists and why `Owned` must be *proven*, never
// presumed.
//
// It is derived, not annotated. `rules/zero-hardcode.md`'s annotation list
// (`@[Primitive]` `@[External]` `@[Literal]`) is closed, and an
// `@[ReturnsOwned]` would fail that file's own test twice over: it states
// what the compiler should *do* with a result rather than what the result
// *is*, and it is computable from the body the compiler already has. So it is
// computed — once, serially, in the shape the same rule sanctions for
// `bConstructs`/`bIndirect`: take the decision where the information is, and
// record it so no later consumer re-derives it.
namespace Volt::Sema::Raii
{

// Stamps `Member::bReturnsOwned` across the whole store.
//
// **Where this must run**: the serial Driver seam, after
// `ResolveUnitSignatures` (so every member exists and is resolvable) and
// before the parallel `TypeChecker` runs (which read the flag concurrently
// and must never see it move). That is the same seam
// `SynthesizeFinalizeStubs` already uses, one step later.
//
// `Units` is indexed by unit ordinal, exactly like `NominalType::Unit` /
// `Member::Unit`. A null entry is a cache-hit stdlib slot: its members carry
// the flag *from the cache* already (`Member` is a reflected aggregate, so it
// serializes with everything else), and its bodies are deliberately not
// re-analysed.
//
// The analysis is a monotone fixpoint — every flag starts `false` and only
// ever flips to `true` — so it terminates, and an unprovable case (mutual
// recursion, an unresolvable callee) simply stays `false`. `false` means
// `Borrowed`, which costs a leak the metrics count rather than a double free
// no metric can undo.
SEMA_EXPORT void InferReturnOwnership ( std::span<const Frontend::AstContext *const> Units, TypeStore &Store );

// Stamps `Member::ParamEscapes` across the whole store — the mirror question,
// asked of an *argument* rather than of a result: does the callee keep what it
// is handed?
//
// Same seam, same `Units` convention, same arbitration, opposite starting
// point. Ownership of a result must be *proven* before the caller releases it,
// so `bReturnsOwned` starts `false`; escape of an argument must be *disproven*
// before the caller releases it, so every slot starts `true` and only ever
// flips to `false`. Both fixpoints are therefore monotone and terminate, and
// both leave an unprovable case on the leak side of the arbitration.
//
// Without it, `arr.push( "x".dup )` is a double free rather than a leak: the
// caller's full-expression region owns the temporary, `Array<T>#push` stores
// it, and the array's own element cascade frees it a second time. That is why
// this runs before anything classifies a `Member` as an invocation
// (`.agents/CASCADE_FINALIZE.md` item 1) — the widening is only safe once the
// callee can say it borrows.
SEMA_EXPORT void InferParameterEscape ( std::span<const Frontend::AstContext *const> Units, TypeStore &Store );

// The single reader of the bit above, for the positional argument at `Index`.
//
// Positional, so `&block` slots are skipped: a block parameter binds through a
// call's trailing `do ... end`, never through the argument list
// (`Member::ParamIsBlock`). Reads `true` — escaping, therefore not the
// caller's to release — for an index past the end and for a member whose
// `ParamEscapes` was never sized. Both readers stay here rather than at their
// call sites so the skip rule cannot drift from the bit it reads.
[[nodiscard]] bool ParameterEscapes ( const Member &Decl, std::size_t Index );

// The same question for the `&block` slot itself — what a trailing
// `do ... end` binds to, and therefore what decides whether the closure
// environment built for it can be released once the call returns.
[[nodiscard]] bool BlockParameterEscapes ( const Member &Decl );

// The same question, asked of a closure *literal* instead of a declared
// member: which of its own parameters does its body keep?
//
// A callable has exactly one member — the `FuncType` claimant's bodyless
// `abstract call`, which declares no parameters at all (a Proc's arity lives
// in its type arguments). So every argument of every indirect call reads as
// escaping, and a desugared composition — `g( f( x ) )`, where `f( x )` is a
// fresh buffer nobody else will ever hold — leaks its intermediate by
// construction. The declaration cannot carry the answer for the same reason it
// cannot carry `bReturnsOwned`: one declaration stands for every closure in
// the program.
//
// So it is asked of the literal, with the identical walk `InferParameterEscape`
// runs over a method body, and the identical arbitration: a slot is `true`
// unless the walk disproves it. `Id` must name a `Lambda` or `Block`; anything
// else yields an empty result, which every reader treats as "escaping".
[[nodiscard]] SEMA_EXPORT Core::SmallVec<bool, 4>
ClosureParameterEscape ( const Frontend::AstContext &Ast, const TypeStore &Store, Frontend::ExprId Id );

} // namespace Volt::Sema::Raii
