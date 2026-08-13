#pragma once

#include "ExprInferencer.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/MiddleEnd/Analysis/Raii/Ownership.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"

// Synthesis of the destruction call itself.
//
// Given "this receiver must be released", this builds the AST that releases
// it. It decides *nothing*: which receivers need releasing, and where, is
// `ScopeCleanup`/`Temporaries`/`FieldCascade`'s business.
//
// It is one call, and it will stay one call. A container's elements are the
// container's own business, written in Volt (`Array<T>#finalize` loops over
// `@buffer` and calls `finalize` on each element) — never a `.size`/`[]` loop
// this file synthesizes, which is what it used to do and which required the
// middle-end to know what a *sequence* is. That knowledge is gone, together
// with the node-kind claim that gated it: the compiler now knows only that
// values are born through `initialize` and die through `finalize`, and
// nothing whatever about what any particular type holds.
//
// The call it emits goes through the ordinary `InferExpr`/`MemberType` path
// that hand-written source would take — never a hand-stamped
// `CalleeResolution` (rules/backend-machine-only.md's "a synthesized operator
// is not a built-in either").
//
// It is a template because the two callers differ in exactly one way: what
// kind of node reads the receiver back. A local reads through an
// `Identifier` bound in scope; a field reads through an `InstanceVar`.
// `MakeReadId` is invoked once per *fresh occurrence* the synthesized tree
// needs — value-AST nodes are single-owner, so a receiver mentioned N times
// needs N distinct nodes (rules/ast-value.md).
namespace Volt::MiddleEnd::Analysis::Lifetime
{

// `<receiver>.finalize()`.
//
// `MakeReadId` is called once, for the single occurrence of the receiver the
// call needs. It stays a callable rather than a plain `ExprId` because the
// two call sites build that occurrence differently, and because the shape
// this used to have needed several of them.
template <typename MakeReadIdFn>
[[nodiscard]] Frontend::ExprId BuildFinalizeCallOnReceiver ( TypeCheckerContext &Context,
                                                             Volt::Core::SourceRange Loc,
                                                             MakeReadIdFn MakeReadId,
                                                             MiddleEnd::TypeSystem::SemaTypeId ValueType )
{
    static_cast<void>( ValueType );

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const Frontend::ExprId ReadId   = MakeReadId();
    const Frontend::ExprId MemberId = Ast.Add( Frontend::ExprNode{ Frontend::Member{
        .Loc = Loc, .Object = ReadId, .Name = Ast.Strings().Intern( MiddleEnd::Analysis::Raii::FinalizeName ) } } );
    const Frontend::ExprId CallId   = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = Loc, .Callee = MemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );
    InferExpr( Context, CallId );
    return CallId;
}

} // namespace Volt::MiddleEnd::Analysis::Lifetime
