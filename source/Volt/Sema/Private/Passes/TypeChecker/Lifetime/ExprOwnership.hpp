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
// **`Owned` is proven, never presumed** (.agents/CASCADE_FINALIZE.md). Two
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
// Two proofs are accepted:
//
//   - `bConstructs` — `T.new( … )` allocates the storage it hands back.
//     Certain by construction, no inference involved.
//   - `Member::bReturnsOwned` — derived by the seam-time fixpoint in
//     `Raii::InferReturnOwnership`, which read the callee's own body.
//
// The `Member`/`Identifier` arm is the split `.agents/CASCADE_FINALIZE.md`
// item 1 called for. Those two node kinds mean *either* a paren-less
// invocation *or* a place read, and the model must not confuse them: a place
// read borrows storage its real owner still holds, so releasing it is a
// double free. The discriminator is what the resolution found — a `Method` is
// an invocation, a `Field` is a place — never the node kind, which cannot
// tell them apart. `bConstructs` is not accepted on this arm either: a
// `T.new` spelling resolves on the `Member` standing in callee position, and
// the value it constructs belongs to the `Call` above it, not to that node.
[[nodiscard]] inline bool ProducesOwnedValue ( TypeCheckerContext &Context, const Frontend::ExprId Id )
{
    const Resolution *Found = ResolutionOf( Context, Id );
    if ( Found == nullptr )
    {
        return false;
    }

    const Frontend::ExprNode &Node = Context.Ctx.Ast.Expr( Id );
    if ( std::holds_alternative<Frontend::Member>( Node ) or std::holds_alternative<Frontend::Identifier>( Node ) )
    {
        return Found->Decl != nullptr and Found->Decl->Kind == EMemberKind::Method and Found->Decl->bReturnsOwned;
    }

    if ( Found->bConstructs )
    {
        return true;
    }
    // An indirect call goes through a callable *value*; the member it
    // resolves to is the `FuncType` claimant's abstract contract, which has
    // no body and so was never proven to return owned. Reading the flag
    // handles that without a special case — and is the whole of
    // `.agents/CASCADE_FINALIZE.md` item 3's first half: through a function
    // pointer the body is genuinely unknown, so the result stays `Borrowed`.
    return Found->Decl != nullptr and Found->Decl->bReturnsOwned;
}

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
