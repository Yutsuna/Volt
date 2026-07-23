#include "DeclStmtWalker.hpp"

#include "Volt/Sema/Layout/TypeResolve.hpp"

void Volt::Sema::TypeCheckerPass::EnterType ( TypeCheckerContext &Context,
                                              NominalId Id,
                                              const Frontend::SymbolList &Params,
                                              const Frontend::DeclList &Body )
{
    const NominalId OuterType                 = Context.SelfType;
    const Frontend::SymbolList *OuterGenerics = Context.SelfGenerics;
    const SemaTypeId OuterValue               = Context.SelfValue;

    Context.SelfType     = Id;
    Context.SelfGenerics = &Params;

    Core::SmallVec<SemaTypeId, 2> Args;
    for ( std::size_t Index = 0; Index < Params.Size(); ++Index )
    {
        Args.PushBack( SemaTypeId{} );
    }
    Context.SelfValue = Context.MakeType( Id, std::move( Args ) );

    WalkDecls( Context, Body );

    Context.SelfType     = OuterType;
    Context.SelfGenerics = OuterGenerics;
    Context.SelfValue    = OuterValue;
}

void Volt::Sema::TypeCheckerPass::EnterMethod ( TypeCheckerContext &Context, const Frontend::Method &Node )
{
    std::unordered_map<BindingSite, SemaTypeId, BindingSiteHash> OuterLocalTypes;
    OuterLocalTypes.swap( Context.LocalTypes );
    std::unordered_map<Symbol, SemaTypeId> OuterLocals;
    OuterLocals.swap( Context.Locals );
    std::unordered_set<std::uint32_t> OuterUnconstrainedLiterals;
    OuterUnconstrainedLiterals.swap( Context.UnconstrainedLiterals );
    std::unordered_map<Symbol, Frontend::ExprId> OuterUnconstrainedVarInitializers;
    OuterUnconstrainedVarInitializers.swap( Context.UnconstrainedVarInitializers );
    const SemaTypeId OuterReturnType = Context.CurrentMethodReturnType;

    UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Context.SelfValue };
    Context.CurrentMethodReturnType =
        ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Node.ReturnType );

    const bool bOuterStatic = Context.bStaticContext;
    Context.bStaticContext  = Node.bSelf;

    for ( const Frontend::ParamId Id : Node.Params )
    {
        const Frontend::Param &Entry = Context.Ctx.Ast.GetParam( Id );
        const SemaTypeId ParamType =
            ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Entry.DeclType );
        Context.LocalTypes[BindingSite{ Id }] = ParamType;
        Context.Locals[Entry.Name]            = ParamType;
        Context.Ctx.Values.SetSiteType( BindingSite{ Id }, ParamType );
        InferExpr( Context, Entry.Default );
    }
    WalkStmts( Context, Node.Body );

    Context.bStaticContext          = bOuterStatic;
    Context.CurrentMethodReturnType = OuterReturnType;
    Context.LocalTypes.swap( OuterLocalTypes );
    Context.Locals.swap( OuterLocals );
    Context.UnconstrainedLiterals.swap( OuterUnconstrainedLiterals );
    Context.UnconstrainedVarInitializers.swap( OuterUnconstrainedVarInitializers );
}

void Volt::Sema::TypeCheckerPass::WalkDecl ( TypeCheckerContext &Context, Frontend::DeclId Id )
{
    if ( not Id.IsValid() )
    {
        return;
    }

    std::visit(
        Meta::Overloaded{
            [&] ( const Frontend::Struct &Node )
            {
                EnterType( Context, Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ),
                           Node.Generics, Node.Body );
            },
            [&] ( const Frontend::Class &Node )
            {
                EnterType( Context, Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ),
                           Node.Generics, Node.Body );
            },
            [&] ( const Frontend::Mixin &Node )
            {
                EnterType( Context, Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ),
                           Node.Generics, Node.Body );
            },
            [&] ( const Frontend::Method &Node ) { EnterMethod( Context, Node ); },
            [] ( const Frontend::Annotation & ) {},
            [&] ( const auto &Node ) { WalkChildren( Context, Node ); },
        },
        Context.Ctx.Ast.Decl( Id ) );
}

void Volt::Sema::TypeCheckerPass::WalkStmts ( TypeCheckerContext &Context, const Frontend::StmtList &Stmts )
{
    for ( const Frontend::StmtId Id : Stmts )
    {
        WalkStmt( Context, Id );
    }
}

void Volt::Sema::TypeCheckerPass::WalkStmt ( TypeCheckerContext &Context, Frontend::StmtId Id )
{
    if ( not Id.IsValid() )
    {
        return;
    }

    std::visit(
        Meta::Overloaded{
            [&] ( const Frontend::LocalDecl &Node )
            {
                UnitSink Sink{ .Values = Context.Ctx.Values, .Self = Context.SelfValue };
                const SemaTypeId Written =
                    ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Node.DeclType );
                if ( Written.IsValid() and Node.Init.IsValid() )
                {
                    Context.ConstrainExprType( Node.Init, Written );
                }
                const SemaTypeId Init                 = InferExpr( Context, Node.Init );
                const SemaTypeId Bound                = Written.IsValid() ? Written : Init;
                Context.LocalTypes[BindingSite{ Id }] = Bound;
                Context.Locals[Node.Name]             = Bound;
                Context.Ctx.Values.SetSiteType( BindingSite{ Id }, Bound );
                if ( not Written.IsValid() and Node.Init.IsValid() )
                {
                    Context.UnconstrainedVarInitializers[Node.Name] = Node.Init;
                }
            },
            [&] ( const Frontend::Return &Node )
            {
                if ( Node.Value.IsValid() and Context.CurrentMethodReturnType.IsValid() )
                {
                    Context.ConstrainExprType( Node.Value, Context.CurrentMethodReturnType );
                }
                WalkChildren( Context, Node );
            },
            [&] ( const auto &Node ) { WalkChildren( Context, Node ); },
        },
        Context.Ctx.Ast.Stmt( Id ) );
}
