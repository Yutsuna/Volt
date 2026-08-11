#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"

// The cleanup boundary — the primitive Volt's RAII model is defined in terms
// of.
//
// A *region* is a span of code that owns some set of values. Its **boundary**
// is the single point every path leaving that region must traverse, and where
// whatever it still owns gets finalized. One boundary per region, however
// many values the region owns — never one per operator, never one per value.
//
// `Frontend::BeginExpr` is how a boundary is *lowered* in the middle-end as
// it stands today; it is not what a boundary *is*. `EmitBegin`
// (BeginRescueEmitter.cpp) already threads fall-through, unhandled `raise`
// and non-local `break` through `EnsureBody` and re-propagates through
// enclosing `begin`s, which is exactly a boundary's contract, so the model
// costs the backend no new node. Should that ever change — a dedicated node,
// a per-frame cleanup table, real landing pads — **this file is the only one
// that has to move.**
//
// That is why constructing a `Frontend::BeginExpr` anywhere else in the RAII
// module is a layering violation rather than a style preference:
//
//     grep -rn 'Frontend::BeginExpr{' source/Volt/Sema/Private/Raii
//     grep -rn 'Frontend::BeginExpr{' source/Volt/Sema/Private/Passes/TypeChecker/Lifetime
//     # → CleanupRegion.cpp only
//
// (*Matching* a `BeginExpr` while walking — `get_if`/`holds_alternative` — is
// a different thing and stays wherever traversal happens: it is a core AST
// node a walker must recognise like `If` or `While`.)
namespace Volt::Sema::TypeCheckerPass::Lifetime
{

// The boundary itself: run `Body`, and on *every* way out of it run
// `CleanupBody`.
//
// `ResultType` is the type the region converges to as a value, copied onto
// the boundary so a region standing in expression position (an `If` branch
// feeding an assignment) still types. Pass an invalid id when the region has
// no value.
[[nodiscard]] Frontend::ExprId EmitBoundary ( Frontend::AstContext &Ast,
                                              UnitTypes &Values,
                                              Core::SourceRange Loc,
                                              Frontend::StmtList Body,
                                              Frontend::StmtList CleanupBody,
                                              SemaTypeId ResultType );

// The same boundary, written *into* an expression slot that already exists
// rather than appended as a fresh one.
//
// A full-expression region is discovered around an expression some parent
// node already points at (`LocalDecl::Init`, `Return::Value`, `While::Cond`).
// Rewriting the slot in place is what lets the region be introduced without
// touching that parent at all — the arena-rewrite discipline
// (rules/ast-rewrite.md) in its purest form: parents refer to the `Id`, so
// the whole tree updates at once.
//
// `Body`'s tail must therefore be whatever the slot used to evaluate to,
// moved to a slot of its own — this function does not move it for the caller,
// because only the caller knows which side tables that move has to carry
// along (`CalleeResolution` for a `Binary`, a scope binding for an
// `Identifier`).
void EmitBoundaryInto ( Frontend::AstContext &Ast,
                        UnitTypes &Values,
                        Frontend::ExprId Slot,
                        Core::SourceRange Loc,
                        Frontend::StmtList Body,
                        Frontend::StmtList CleanupBody,
                        SemaTypeId ResultType );

// Several statements sequenced into one expression — **not** a boundary: it
// owns nothing and cleans up nothing.
//
// It shares `BeginExpr` as its lowering purely because "statements in
// expression position" is the other thing that node can express, which is
// why it lives here rather than open-coding the node at its call site.
[[nodiscard]] Frontend::ExprId EmitSequence (
    Frontend::AstContext &Ast, UnitTypes &Values, Core::SourceRange Loc, Frontend::StmtList Body, SemaTypeId ResultType );

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
