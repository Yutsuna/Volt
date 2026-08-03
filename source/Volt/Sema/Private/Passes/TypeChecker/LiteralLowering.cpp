#include "LiteralLowering.hpp"

#include "DeclStmtWalker.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"

void Volt::Sema::TypeCheckerPass::LowerArrayLit ( TypeCheckerContext &Context,
                                                  Frontend::ExprId Id,
                                                  Frontend::ExprList Elements,
                                                  SemaTypeId LiteralType )
{
    if ( not LiteralType.IsValid() or not Context.Ctx.Values.Has( LiteralType ) )
    {
        return;
    }

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // The receiver's own SemaTypeId is already known — the literal's own
    // type, already instantiated with its concrete element type — so the
    // Object child is stamped directly rather than rebuilt from a
    // source-level GenericInst. LookupOn/MemberType still perform the
    // ordinary member resolution for `new`; only the receiver's *type* comes
    // from Sema's own value rather than re-deriving it from a name.
    const NominalId Base            = Context.Ctx.Values.Get( LiteralType ).Base;
    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Base ).Name );
    const Frontend::Symbol NameSym  = Ast.Strings().Intern( NameText );

    const Frontend::ExprId ObjectId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = NameSym } } );
    Context.Ctx.Values.SetExprType( ObjectId, LiteralType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId =
        Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );
    const Frontend::ExprId CtorId = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = {}, .Callee = NewMemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );

    // `tmp = T.new()` — an ordinary implicit local declaration (Assign onto
    // an Identifier ScopeResolver never bound), exactly the shape `arr =
    // Array.new()` written by hand takes. TypeCheckerContext::WriteLocal
    // falls back to the flat, name-keyed Locals map for exactly this case —
    // "a node minted after Order 10" (TypeCheckerContext.cpp).
    const Frontend::Symbol TmpName   = Ast.MakeUniqueSymbol( "__array_lit" );
    const Frontend::ExprId TmpTarget = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    const Frontend::ExprId AssignId =
        Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = {}, .Target = TmpTarget, .Value = CtorId } } );

    Frontend::StmtList Body;
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = AssignId } } ) );

    // One `tmp << element` per element, resolved through the same operator
    // path (MemberType) any hand-written `tmp << element` would take — no
    // shortcut past member resolution (rules/backend-machine-only.md). A
    // type claiming ArrayLit but declaring no `<<` fails this exactly as an
    // unresolved method call always does; no diagnostic of its own.
    for ( const Frontend::ExprId Elem : Elements )
    {
        const Frontend::ExprId TmpUse   = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
        const Frontend::ExprId AppendId = Ast.Add(
            Frontend::ExprNode{ Frontend::Binary{ .Loc = {}, .Op = Frontend::TokenKind::Shl, .Lhs = TmpUse, .Rhs = Elem } } );
        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = AppendId } } ) );
    }

    const Frontend::ExprId TrailingUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = TrailingUse } } ) );

    // Type every synthesized statement off the local `Body` copy — never off
    // a re-read of the arena slot at `Id`, which WalkStmt's own Add() calls
    // (constructor/operator resolution, generic instantiation) could
    // reallocate out from under a reference (rules/ast-rewrite.md).
    for ( const Frontend::StmtId StmtId : Body )
    {
        WalkStmt( Context, StmtId );
    }

    // Copy-out (Elements, above) / compute (WalkStmt, above) / write-back:
    // the arena slot is assigned only now, after every Add() this rewrite
    // performs has already happened.
    Ast.Expr( Id ) =
        Frontend::ExprNode{ Frontend::BeginExpr{ .Loc = {}, .Body = std::move( Body ), .RescueClauses = {}, .EnsureBody = {} } };
}

void Volt::Sema::TypeCheckerPass::LowerArrayLits ( TypeCheckerContext &Context )
{
    // Bound before the first Add() — every node this rewrite creates lands
    // past OriginalCount and is never itself an ArrayLit, so skipping it is
    // deliberate (rules/ast-rewrite.md). A nested literal was parsed first,
    // so it always carries a smaller index than the one containing it: the
    // sweep processes it — and rewrites it into a BeginExpr — before the
    // outer literal's own turn, exactly the innermost-first order other
    // lowerings rely on.
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();
    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        // A metadata expression (an `@[...]` annotation argument, a macro
        // invocation's own arguments) is never evaluated and never typed —
        // InferExpr short-circuits it on sight. Lowering it into runtime
        // construction code would build something nothing reads.
        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::ArrayLit>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        // Copied out before Add(): std::get, like std::visit, would otherwise
        // hand back a reference straight into the arena slot this same call
        // goes on to rewrite (rules/ast-rewrite.md).
        const Frontend::ExprList Elements = std::get<Frontend::ArrayLit>( Context.Ctx.Ast.Expr( Id ) ).Elements;

        LowerArrayLit( Context, Id, Elements, Context.Ctx.Values.ExprType( Id ) );
    }
}

void Volt::Sema::TypeCheckerPass::LowerHashLit (
    TypeCheckerContext &Context, Frontend::ExprId Id, Frontend::ExprList Keys, Frontend::ExprList Values, SemaTypeId LiteralType )
{
    if ( not LiteralType.IsValid() or not Context.Ctx.Values.Has( LiteralType ) )
    {
        return;
    }

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const NominalId Base            = Context.Ctx.Values.Get( LiteralType ).Base;
    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Base ).Name );
    const Frontend::Symbol NameSym  = Ast.Strings().Intern( NameText );

    const Frontend::ExprId ObjectId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = NameSym } } );
    Context.Ctx.Values.SetExprType( ObjectId, LiteralType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId =
        Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );
    const Frontend::ExprId CtorId = Ast.Add(
        Frontend::ExprNode{ Frontend::Call{ .Loc = {}, .Callee = NewMemberId, .Args = {}, .ArgNames = {}, .BlockArg = {} } } );

    const Frontend::Symbol TmpName   = Ast.MakeUniqueSymbol( "__hash_lit" );
    const Frontend::ExprId TmpTarget = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    const Frontend::ExprId AssignId =
        Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = {}, .Target = TmpTarget, .Value = CtorId } } );

    Frontend::StmtList Body;
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = AssignId } } ) );

    const Frontend::Symbol SetOpSym = Ast.Strings().Intern( "[]=" );
    const std::size_t PairCount     = std::min( Keys.Size(), Values.Size() );

    for ( std::size_t Index = 0; Index < PairCount; ++Index )
    {
        const Frontend::ExprId TmpUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
        const Frontend::ExprId SetMemberId =
            Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = TmpUse, .Name = SetOpSym } } );

        Frontend::ExprList Args;
        Args.PushBack( Keys[Index] );
        Args.PushBack( Values[Index] );

        Frontend::SymbolList ArgNames;
        ArgNames.PushBack( Core::Symbol{} );
        ArgNames.PushBack( Core::Symbol{} );

        const Frontend::ExprId IndexSetCallId = Ast.Add( Frontend::ExprNode{
            Frontend::Call{ .Loc = {}, .Callee = SetMemberId, .Args = Args, .ArgNames = ArgNames, .BlockArg = {} } } );

        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = IndexSetCallId } } ) );
    }

    const Frontend::ExprId TrailingUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = TmpName } } );
    Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = {}, .Expr = TrailingUse } } ) );

    for ( const Frontend::StmtId StmtId : Body )
    {
        WalkStmt( Context, StmtId );
    }

    Ast.Expr( Id ) =
        Frontend::ExprNode{ Frontend::BeginExpr{ .Loc = {}, .Body = std::move( Body ), .RescueClauses = {}, .EnsureBody = {} } };
}

void Volt::Sema::TypeCheckerPass::LowerHashLits ( TypeCheckerContext &Context )
{
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();
    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::HashLit>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        const Frontend::HashLit HashLitNode = std::get<Frontend::HashLit>( Context.Ctx.Ast.Expr( Id ) );
        const Frontend::ExprList Keys       = HashLitNode.Keys;
        const Frontend::ExprList Values     = HashLitNode.Values;

        LowerHashLit( Context, Id, Keys, Values, Context.Ctx.Values.ExprType( Id ) );
    }
}
