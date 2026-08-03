#include "ClosureLifting.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"

void Volt::Sema::TypeCheckerPass::LowerClosureLit ( TypeCheckerContext &Context, Frontend::ExprId Id )
{
    const SemaTypeId LiteralType = Context.Ctx.Values.ExprType( Id );
    if ( not LiteralType.IsValid() or not Context.Ctx.Values.Has( LiteralType ) )
    {
        return;
    }

    // 3a scope: a capturing closure keeps its Lambda/Block shape until Phase
    // 3b adds the Pointer<UInt8>-arithmetic env rewrite
    // (.agents/PLAN_CLOSURE_LOWERING.md).
    if ( const ScopeId ClosureScope = Context.Ctx.Scopes.ScopeOfExpr( Id ); ClosureScope.IsValid() )
    {
        if ( const auto *Captures = Context.Ctx.Scopes.CapturesOf( ClosureScope );
             Captures != nullptr and not Captures->IsEmpty() )
        {
            return;
        }
    }

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Copied out before any Add(): the source variant is read once, by
    // value, before this rewrite starts appending to the same arenas
    // (rules/ast-rewrite.md).
    const bool bIsLambda = std::holds_alternative<Frontend::Lambda>( Ast.Expr( Id ) );

    Core::SourceRange Loc;
    Frontend::ParamList Params;
    Frontend::StmtList Body;
    if ( bIsLambda )
    {
        const Frontend::Lambda Node = std::get<Frontend::Lambda>( Ast.Expr( Id ) );
        Loc                         = Node.Loc;
        Params                      = Node.Params;
        // `(x) => expr` has a single expression body; a synthesized Method
        // needs a StmtList, so the expression becomes its sole, trailing
        // ExprStmt — the same implicit-return shape any ordinary def's
        // trailing expression already carries.
        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Node.Loc, .Expr = Node.Body } } ) );
    }
    else
    {
        const Frontend::Block Node = std::get<Frontend::Block>( Ast.Expr( Id ) );
        Loc                        = Node.Loc;
        Params                     = Node.Params;
        Body                       = Node.Body;
    }

    // The lifted function's own name never has to resolve — it is reached
    // only through the FuncAddr below — so a unique symbol is enough.
    Frontend::Method Synth;
    Synth.Loc    = Loc;
    Synth.Name   = Ast.MakeUniqueSymbol( "__closure" );
    Synth.Params = Params;
    Synth.Body   = Body;

    const Frontend::DeclId NewDecl = Ast.Add( Frontend::DeclNode{ std::move( Synth ) } );
    Ast.TopDecls.push_back( NewDecl );

    // ClosureType's own encoding: Args[0] is the result, Args[1..] are the
    // parameters, in declaration order (ClosureInferencer.cpp).
    const SemaType &Closure = Context.Ctx.Values.Get( LiteralType );
    const SemaTypeId Result = Closure.Args.IsEmpty() ? SemaTypeId{} : Closure.Args[0];
    Core::SmallVec<SemaTypeId, 4> ParamTypes;
    for ( std::size_t Index = 1; Index < Closure.Args.Size(); ++Index )
    {
        ParamTypes.PushBack( Closure.Args[Index] );
    }
    Context.Ctx.Synth.Add( SynthesizedFunction{ .Decl = NewDecl, .Result = Result, .Params = std::move( ParamTypes ) } );

    // `Proc.new( FuncAddr, nil )`, receiver hand-stamped to the literal's own
    // (bespoke multi-arg) type — the same technique LowerArrayLit/
    // LowerStringLit already use, and for the same reason: ordinary generic
    // instantiation could never reproduce ClosureType's arity against
    // Proc<R>'s single declared parameter (finding #1,
    // .agents/PLAN_CLOSURE_LOWERING.md).
    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Closure.Base ).Name );
    const Frontend::Symbol NameSym  = Ast.Strings().Intern( NameText );

    const Frontend::ExprId ObjectId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = NameSym } } );
    Context.Ctx.Values.SetExprType( ObjectId, LiteralType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId = Ast.Add(
        Frontend::ExprNode{ Frontend::Member{ .Loc = Loc, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );

    const Frontend::ExprId FuncAddrId = Ast.Add( Frontend::ExprNode{ Frontend::FuncAddr{ .Loc = Loc, .Target = NewDecl } } );
    const Frontend::ExprId EnvId      = Ast.Add( Frontend::ExprNode{ Frontend::NilLiteral{ .Loc = Loc } } );

    Frontend::ExprList Args;
    Args.PushBack( FuncAddrId );
    Args.PushBack( EnvId );
    Frontend::SymbolList ArgNames;
    ArgNames.PushBack( Core::Symbol{} );
    ArgNames.PushBack( Core::Symbol{} );

    const Frontend::ExprId CtorCallId = Ast.Add( Frontend::ExprNode{
        Frontend::Call{ .Loc = Loc, .Callee = NewMemberId, .Args = Args, .ArgNames = ArgNames, .BlockArg = {} } } );

    InferExpr( Context, CtorCallId );

    if ( const auto Entry = Context.CalleeResolution.find( CtorCallId.Value ); Entry != Context.CalleeResolution.end() )
    {
        Context.CalleeResolution[Id.Value] = Entry->second;
    }
    if ( const SemaTypeId CallTypeRes = Context.Ctx.Values.ExprType( CtorCallId ); CallTypeRes.IsValid() )
    {
        Context.Ctx.Values.SetExprType( Id, CallTypeRes );
    }

    // Copy-out (Params/Body, above) / compute (InferExpr, above) /
    // write-back: the arena slot is assigned only now, after every Add()
    // this rewrite performs has already happened (rules/ast-rewrite.md).
    Ast.Expr( Id ) = Ast.Expr( CtorCallId );
}

void Volt::Sema::TypeCheckerPass::LowerClosureLits ( TypeCheckerContext &Context )
{
    // Bound before the first Add(): every node this rewrite creates lands
    // past OriginalCount and is never itself a Lambda/Block
    // (rules/ast-rewrite.md).
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();
    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::Lambda>( Context.Ctx.Ast.Expr( Id ) ) and
             not std::holds_alternative<Frontend::Block>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        LowerClosureLit( Context, Id );
    }
}
