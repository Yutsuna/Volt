#pragma once

#include "../TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstdint>
#include <variant>

// The classification table of the ownership model, at the level of one
// *expression*: does evaluating it hand the surrounding region a value that
// region must now release?
//
// `Raii/Ownership.hpp` answers the same family of questions about a *type*
// ("does this need finalizing at all"); this answers it about an occurrence,
// which needs the resolution `TypeChecker` recorded and therefore a
// `TypeCheckerContext`. Header-only and shared, because `ScopeCleanup` (which
// decides whether a local owns its initializer) and `Temporaries` (which
// decides whether an unnamed value owns itself) must not drift: they are the
// two halves of one rule.
//
// **`Owned` is proven, never presumed** (rules/raii-ownership.md). Two
// proofs are accepted and no third — a construction, and a callee the
// seam-time fixpoint read a body for. Everything else reads as `Borrowed`,
// which costs a counted leak instead of a double free.
namespace Volt::Sema::TypeCheckerPass::Lifetime
{

// Where a node's callee resolution is keyed.
//
// `Call` records against its *callee*'s id. Every other invocation shape
// records against its own: `MemberType` is handed the operator node itself
// for `Binary`/`Unary` (ExprInferencer.cpp), and a paren-less invocation has
// no call node at all — `a.dup + b` parses as `Binary( Member( a, dup ), b )`,
// and a bare name resolving to a method of `self` or to a top-level `def` is
// a plain `Identifier`. Both of those last two are the very node kinds a
// *place read* also uses, which is why reading the resolution is not enough
// on its own; see `ProducesOwnedValue`.
//
// `InstanceVar` is deliberately absent: `@x` carries a resolution too, but it
// names a field by construction and can never be an invocation.
[[nodiscard]] inline const Resolution *ResolutionOf ( TypeCheckerContext &Context, const Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return nullptr;
    }
    const Frontend::ExprNode &Node = Context.Ctx.Ast.Expr( Id );

    std::uint32_t Key = Id.Value;
    if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
    {
        if ( not CallNode->Callee.IsValid() )
        {
            return nullptr;
        }
        Key = CallNode->Callee.Value;
    }
    else if ( not std::holds_alternative<Frontend::Binary>( Node ) and not std::holds_alternative<Frontend::Unary>( Node ) and
              not std::holds_alternative<Frontend::Member>( Node ) and not std::holds_alternative<Frontend::Identifier>( Node ) )
    {
        return nullptr;
    }

    const auto Found = Context.CalleeResolution.find( Key );
    return Found != Context.CalleeResolution.end() ? &Found->second : nullptr;
}

// Does evaluating `Id` *produce* a value whose release is now the enclosing
// region's responsibility?
//
// Exactly two questions, in this order, and no third:
//
//  1. **Is this occurrence an invocation, or a place read?** `Call`,
//     `Binary` and `Unary` are invocations by construction — none of them can
//     name storage. `Member` and `Identifier` are the two spellings that mean
//     *either*, and the model must not confuse them: a place read borrows
//     storage its real owner still holds, so releasing it is a double free.
//     The discriminator is what the resolution found — a `Method` runs a
//     body, a `Field` names storage — never the node kind, which cannot tell
//     them apart. This is the split rules/raii-ownership.md
//     called for.
//  2. **Does that invocation hand back a value nobody else holds?** Two
//     proofs are accepted:
//       - `bConstructs` — `T.new( … )` allocates the storage it returns.
//         Certain by construction, no inference involved.
//       - `Member::bReturnsOwned` — derived by the seam-time fixpoint in
//         `Raii::InferReturnOwnership`, which read the callee's own body.
//     Anything else reads as `Borrowed`, which costs a counted leak instead
//     of a double free.
//
// **Callee position is not this function's business.** `T.new` spelled
// without parentheses *is* the construction; spelled with them, the same
// resolution also sits on the `Member` standing in the `Call`'s callee slot,
// and the value belongs to the `Call`. Discriminating those here would take a
// parent pointer the value AST does not have — and does not need to, because
// a callee is reached only through `Call::Callee`, which both callers already
// treat as `Moved` for an unrelated and stronger reason (the backend keys the
// resolution on that very id). Whoever asks must therefore never ask about a
// callee, and neither of the two does.
[[nodiscard]] inline bool ProducesOwnedValue ( TypeCheckerContext &Context, const Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }

    // A construction the middle-end performed itself, whose shape no longer
    // shows it (`TypeCheckerContext::OwnedExprSites` — a lowered `[ … ]` or
    // `{ … }`, whose slot is a `BeginExpr` ending on a name).
    if ( Context.OwnedExprSites.contains( Id.Value ) )
    {
        return true;
    }

    // A third proof, and the only one that is not a fact about a *declaration*:
    // the callee is a closure literal whose body this unit read
    // (`TypeCheckerContext::OwnedClosureLiterals`). It has to be keyed by the
    // callee's site rather than recorded on a `Member`, because a callable's
    // only member is the `FuncType` claimant's bodyless `abstract call` — one
    // declaration standing for every closure in the program, which can carry
    // no per-closure fact at all.
    if ( const auto *CallNode = std::get_if<Frontend::Call>( &Context.Ctx.Ast.Expr( Id ) ) )
    {
        if ( CallNode->Callee.IsValid() and Context.OwnedClosureLiterals.contains( CallNode->Callee.Value ) )
        {
            return true;
        }
    }

    const Resolution *Found = ResolutionOf( Context, Id );
    if ( Found == nullptr )
    {
        return false;
    }

    const Frontend::ExprNode &Node = Context.Ctx.Ast.Expr( Id );
    const bool bMayNamePlace =
        std::holds_alternative<Frontend::Member>( Node ) or std::holds_alternative<Frontend::Identifier>( Node );
    if ( bMayNamePlace and ( Found->Decl == nullptr or Found->Decl->Kind != EMemberKind::Method ) )
    {
        return false;
    }

    // An indirect call goes through a callable *value*; the member it
    // resolves to is the `FuncType` claimant's abstract contract, which has
    // no body and so was never proven to return owned. Reading the flag
    // handles that without a special case (rules/raii-ownership.md): through a function
    // pointer the body is genuinely unknown, so the result stays `Borrowed`.
    return Found->bConstructs or ( Found->Decl != nullptr and Found->Decl->bReturnsOwned );
}

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
