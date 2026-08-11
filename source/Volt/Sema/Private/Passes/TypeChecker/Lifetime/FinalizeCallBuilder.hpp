#pragma once

#include "../ExprInferencer.hpp"
#include "../TypeCheckerContext.hpp"
#include "CleanupRegion.hpp"
#include "Raii/Ownership.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <utility>

// Synthesis of the `finalize` call tree itself.
//
// Given "this receiver must be released", this builds the AST that releases
// it — including the element loop when the receiver is a sequence holding
// finalize-candidates. It decides *nothing*: which receivers need releasing,
// and where, is `ScopeCleanup`/`Temporaries`/`FieldCascade`'s business.
//
// Every call it synthesizes goes through the ordinary `InferExpr`/`MemberType`
// path that hand-written source would take — never a hand-stamped
// `CalleeResolution` (rules/backend-machine-only.md's "a synthesized operator
// is not a built-in either").
//
// It is a template because the two callers differ in exactly one way: what
// kind of node reads the receiver back. A local reads through an
// `Identifier` bound in scope; a field reads through an `InstanceVar`.
// `MakeReadId` is invoked once per *fresh occurrence* the synthesized tree
// needs — value-AST nodes are single-owner, so a receiver mentioned N times
// needs N distinct nodes (rules/ast-value.md).
namespace Volt::Sema::TypeCheckerPass::Lifetime
{

// The element type a *sequence* receiver cascades into — see
// `Raii::ContainerElementType` (Private/Raii/Ownership.cpp) for why the gate
// is the sequence type's own node-kind claim rather than "has a generic
// argument that declares finalize".
[[nodiscard]] inline Sema::SemaTypeId GetElementFinalizeCandidateType ( TypeCheckerContext &Context, Sema::SemaTypeId LocalType )
{
    return Sema::Raii::ContainerElementType( Context.Ctx.Types, Context.Ctx.Values, LocalType );
}

// `<receiver>.finalize()`, cascading into `<receiver>[i].finalize()` first
// when the receiver's own type carries a finalize-candidate element type
// (`GetElementFinalizeCandidateType` — Array<T>'s own T, resolved the same
// structural way any other candidacy is: no bound, no Volt name spelled
// here, just "does the element type declare finalize"). `MakeReadId` is
// called once per fresh occurrence of the receiver the synthesized tree
// needs (size read, each index read, the final finalize call) — value-AST
// nodes are single-owner, so a receiver used N times needs N distinct nodes
// (rules/ast-value.md). Shared by both a local/rescue candidate
// (`BuildFinalizeCall`, an Identifier read through scope Resolve/BindUse)
// and a field (`BuildFieldFinalizeCall`, an InstanceVar read) — the two
// differ only in what kind of node MakeReadId builds.
template <typename MakeReadIdFn>
[[nodiscard]] Frontend::ExprId BuildFinalizeCallOnReceiver ( TypeCheckerContext &Context,
                                                             Core::SourceRange Loc,
                                                             MakeReadIdFn MakeReadId,
                                                             Sema::SemaTypeId ValueType )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const Sema::SemaTypeId ElemType = GetElementFinalizeCandidateType( Context, ValueType );
    if ( ElemType.IsValid() )
    {
        // A fresh Scope per synthesized loop (Parent invalid — `__fin_i` is
        // never looked up by name, only ever read back through the Binding
        // captured below, so it needs no lexical parent chain). Every
        // occurrence of `__fin_i` must BindUse the *same* Binding — that is
        // what lets codegen's SlotFor (EmitAddress, ExprPlaceEmitter.cpp)
        // find one persistent stack slot for all of them; an Identifier
        // reaching codegen with no Binding at all is refused loudly there
        // ("reached codegen with no scope binding"), which is exactly what
        // silently never fired here before: this whole branch used to be
        // gated on `Types.LookupNodeKind( "UInt64" )`, a category error
        // (LookupNodeKind answers "which type claims this *AST node kind*",
        // e.g. `IntLiteral`/`PointerType` — never "which type is named
        // this"; no type claims a node kind literally spelled `UInt64`, so
        // the lookup always failed and this entire cascade was dead code).
        const Sema::ScopeId LoopScope   = Context.Ctx.Scopes.PushScope( Sema::ScopeId{}, Sema::EScopeKind::Branch );
        const Core::Symbol IdxName      = Ast.MakeUniqueSymbol( "__fin_i" );
        const Sema::Binding *IdxBinding = nullptr;

        auto MakeIdxReadId = [&] () -> Frontend::ExprId
        {
            const Frontend::ExprId Id = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = IdxName } } );
            if ( IdxBinding != nullptr )
            {
                Context.Ctx.Scopes.BindUse( Id, *IdxBinding, true );
            }
            return Id;
        };

        // 1. Init: __fin_i = 0 — the literal carries no suffix and gets no
        // manual type override: it types the same way any other synthesized
        // IntLiteral in this codebase does (ClosureLifting's RunningOffset/
        // SizeOfType — whatever type claims the `IntLiteral` node kind,
        // i.e. Int32), and `candidate.size`/`[]` are UInt64-shaped, so the
        // comparison and index both go through the ordinary same-family
        // widening rule (`rules/zero-hardcode.md`'s "Implicit widening
        // between scalars of the same family") rather than a hand-picked
        // UInt64 nominal.
        const Frontend::ExprId ZeroLitId =
            Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( "0" ) } } );
        InferExpr( Context, ZeroLitId );

        const Frontend::ExprId IdxTargetId = MakeIdxReadId(); // declaring occurrence — IdxBinding is still null here
        Context.Ctx.Scopes.Declare( LoopScope, IdxName, Sema::BindingSite{ IdxTargetId } );
        IdxBinding = Context.Ctx.Scopes.Resolve( LoopScope, IdxName );
        if ( IdxBinding != nullptr )
        {
            Context.Ctx.Scopes.BindUse( IdxTargetId, *IdxBinding, false ); // a write, not a use — ScopeTable.hpp's own convention
        }

        const Frontend::ExprId InitAssignId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = IdxTargetId, .Value = ZeroLitId } } );
        InferExpr( Context, InitAssignId );
        const Frontend::StmtId InitStmtId =
            Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = InitAssignId } } );

        // 2. Condition: __fin_i < candidate.size
        const Frontend::ExprId ReadIdSize = Ast.Add( Frontend::ExprNode{
            Frontend::Member{ .Loc = Loc, .Object = MakeReadId(), .Name = Ast.Strings().Intern( "size" ) } } );
        InferExpr( Context, ReadIdSize );

        const Frontend::ExprId CondId = Ast.Add( Frontend::ExprNode{
            Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Lt, .Lhs = MakeIdxReadId(), .Rhs = ReadIdSize } } );
        InferExpr( Context, CondId );

        // 3. Body statement 1: candidate[__fin_i].finalize() — built directly
        // in `Index`'s own already-desugared form (`Call( Member( x, "[]" ),
        // [ i ] )`, IndexLowering.cpp): `Index` is `VOLT_EXPR_SUGAR`
        // (rules/core-ast.md), a `Lowering`-kind pass rewrites it before
        // TypeChecker runs, and this synthesis happens *inside* TypeChecker
        // — a raw `Index` node here would survive to `AstInvariant` and be
        // refused as residual sugar.
        const Frontend::ExprId IndexArgsId   = MakeIdxReadId();
        const Frontend::ExprId IndexCalleeId = Ast.Add(
            Frontend::ExprNode{ Frontend::Member{ .Loc = Loc, .Object = MakeReadId(), .Name = Ast.Strings().Intern( "[]" ) } } );
        const Frontend::ExprId ElementId = Ast.Add( Frontend::ExprNode{
            Frontend::Call{ .Loc = Loc, .Callee = IndexCalleeId, .Args = { IndexArgsId }, .ArgNames = {}, .BlockArg = {} } } );
        InferExpr( Context, ElementId );

        const Frontend::ExprId ElemMemberId = Ast.Add( Frontend::ExprNode{
            Frontend::Member{ .Loc = Loc, .Object = ElementId, .Name = Ast.Strings().Intern( Sema::Raii::FinalizeName ) } } );
        const Frontend::ExprId ElemCallId   = Ast.Add( Frontend::ExprNode{
            Frontend::Call{ .Loc = Loc, .Callee = ElemMemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );
        InferExpr( Context, ElemCallId );
        const Frontend::StmtId ElemStmtId = Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = ElemCallId } } );

        // 4. Body statement 2: __fin_i = __fin_i + 1
        const Frontend::ExprId OneLitId =
            Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( "1" ) } } );
        InferExpr( Context, OneLitId );

        const Frontend::ExprId AddId = Ast.Add( Frontend::ExprNode{
            Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Plus, .Lhs = MakeIdxReadId(), .Rhs = OneLitId } } );
        InferExpr( Context, AddId );

        const Frontend::ExprId IncrAssignId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = MakeIdxReadId(), .Value = AddId } } );
        InferExpr( Context, IncrAssignId );
        const Frontend::StmtId IncrStmtId =
            Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = IncrAssignId } } );

        // 5. While loop: while __fin_i < candidate.size ... end
        Frontend::StmtList LoopBody;
        LoopBody.PushBack( ElemStmtId );
        LoopBody.PushBack( IncrStmtId );

        const Frontend::StmtId WhileStmtId =
            Ast.Add( Frontend::StmtNode{ Frontend::While{ .Loc = Loc, .Cond = CondId, .Body = std::move( LoopBody ) } } );

        // 6. Finalize candidate call: candidate.finalize()
        const Frontend::ExprId MemberId = Ast.Add( Frontend::ExprNode{
            Frontend::Member{ .Loc = Loc, .Object = MakeReadId(), .Name = Ast.Strings().Intern( Sema::Raii::FinalizeName ) } } );
        const Frontend::ExprId CallId   = Ast.Add(
            Frontend::ExprNode{ Frontend::Call{ .Loc = Loc, .Callee = MemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );
        InferExpr( Context, CallId );
        const Frontend::StmtId FinalizeStmtId = Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } );

        // 7. The three statements sequenced into one expression. Not a
        // cleanup boundary — nothing is owned here, this is only "several
        // statements where an expression is expected" (CleanupRegion.hpp).
        Frontend::StmtList SequenceBody;
        SequenceBody.PushBack( InitStmtId );
        SequenceBody.PushBack( WhileStmtId );
        SequenceBody.PushBack( FinalizeStmtId );

        return EmitSequence( Ast, Context.Ctx.Values, Loc, std::move( SequenceBody ), Context.Ctx.Values.ExprType( CallId ) );
    }

    const Frontend::ExprId ReadId   = MakeReadId();
    const Frontend::ExprId MemberId = Ast.Add( Frontend::ExprNode{
        Frontend::Member{ .Loc = Loc, .Object = ReadId, .Name = Ast.Strings().Intern( Sema::Raii::FinalizeName ) } } );
    const Frontend::ExprId CallId   = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = Loc, .Callee = MemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );
    InferExpr( Context, CallId );
    return CallId;
}

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
