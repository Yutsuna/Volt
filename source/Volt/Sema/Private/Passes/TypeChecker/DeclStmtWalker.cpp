#include "DeclStmtWalker.hpp"

#include "MemberResolver.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <algorithm>
#include <vector>

namespace
{

using namespace Volt;
using namespace Volt::Sema;

// Every mixin reachable through `include`, transitively and without
// repeats. The superclass is deliberately not followed: whatever it
// includes, it was itself required to implement.
void CollectIncludes ( const TypeStore &Store, NominalId Id, std::vector<NominalId> &Out, std::uint32_t Depth )
{
    if ( not Id.IsValid() or Depth > 16 )
    {
        return;
    }

    for ( const SigTypeId Mixin : Store.Type( Id ).Includes )
    {
        const NominalId Base = Store.BaseOf( Mixin );
        if ( not Base.IsValid() or std::ranges::find( Out, Base ) != Out.end() )
        {
            continue;
        }
        Out.push_back( Base );
        CollectIncludes( Store, Base, Out, Depth + 1 );
    }
}

// Every `abstract def` an included mixin declares must resolve, from
// this type, onto something that actually has a body. Resolution goes
// through the ordinary member lookup, so an implementation inherited
// from a superclass or supplied by another mixin counts.
void CheckAbstractConformance ( TypeCheckerPass::TypeCheckerContext &Context, NominalId Id, Core::SourceRange Loc )
{
    std::vector<NominalId> Mixins;
    CollectIncludes( Context.Ctx.Types, Id, Mixins, 0 );

    for ( const NominalId Mixin : Mixins )
    {
        for ( const Member &Entry : Context.Ctx.Types.Type( Mixin ).Members )
        {
            if ( not Entry.bAbstract )
            {
                continue;
            }

            const std::string_view Name = Context.Ctx.Types.Text( Entry.Name );

            // On a primitive, `+` is a machine instruction, not a body the
            // stdlib could ever write. Same exemption as the unknown-member
            // diagnostic, through the same predicate.
            if ( TypeCheckerPass::IsBuiltinOpOn( Context, Id, Name ) )
            {
                continue;
            }

            const InstantiatedMember Found =
                LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, Context.SelfValue, Context.SelfValue, Name );
            if ( Found.Decl != nullptr and not Found.Decl->bAbstract )
            {
                continue;
            }

            Context.Report( Loc.Head(), "type " + Context.NameOf( Id ) + " does not implement abstract member '" +
                                            std::string{ Name } + "' required by mixin " + Context.NameOf( Mixin ) );
        }
    }
}

} // namespace

void Volt::Sema::TypeCheckerPass::EnterType ( TypeCheckerContext &Context,
                                              NominalId Id,
                                              const Frontend::SymbolList &Params,
                                              const Frontend::DeclList &Body,
                                              Core::SourceRange Loc,
                                              bool bConcrete )
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

    if ( bConcrete )
    {
        CheckAbstractConformance( Context, Id, Loc );
    }

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
    std::unordered_set<Symbol> OuterUninitializedLocals;
    OuterUninitializedLocals.swap( Context.UninitializedLocals );
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
    Context.UninitializedLocals.swap( OuterUninitializedLocals );
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
                           Node.Generics, Node.Body, Node.Loc, true );
            },
            [&] ( const Frontend::Class &Node )
            {
                EnterType( Context, Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ),
                           Node.Generics, Node.Body, Node.Loc, true );
            },
            [&] ( const Frontend::Mixin &Node )
            {
                EnterType( Context, Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ),
                           Node.Generics, Node.Body, Node.Loc, false );
            },
            [&] ( const Frontend::Enum &Node )
            {
                EnterType( Context, Context.Ctx.Types.LookupType( Context.Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ),
                           Node.Generics, Node.Body, Node.Loc, true );
            },
            [&] ( const Frontend::Method &Node ) { EnterMethod( Context, Node ); },
            [] ( const Frontend::Annotation & ) {},
            [&] ( const auto &Node ) { WalkChildren( Context, Node ); },
        },
        Context.Ctx.Ast.Decl( Id ) );
}

void Volt::Sema::TypeCheckerPass::WalkStmts ( TypeCheckerContext &Context, const Frontend::StmtList &Stmts )
{
    for ( std::size_t Index = 0; Index < Stmts.Size(); ++Index )
    {
        const Frontend::StmtId Id = Stmts[Index];
        WalkStmt( Context, Id );

        // Detect unreachable code: if this statement is a control-flow
        // interrupt (return, raise, break, next) and there are more
        // statements after it in the same block, nothing below can execute.
        if ( not Id.IsValid() )
        {
            continue;
        }
        const bool bDiverges = std::visit(
            Meta::Overloaded{
                [] ( const Frontend::Return & ) { return true; },
                [] ( const Frontend::Break & ) { return true; },
                [] ( const Frontend::Next & ) { return true; },
                [] ( const Frontend::ExprStmt &S )
                {
                    // A bare `raise` is an ExprStmt wrapping a RaiseExpr.
                    static_cast<void>( S );
                    return false;
                },
                [] ( const auto & ) { return false; },
            },
            Context.Ctx.Ast.Stmt( Id ) );

        // Check the ExprStmt case for `raise`: the expression type is NoReturn.
        const bool bExprDiverges = [&]
        {
            if ( const auto *Stmt = std::get_if<Frontend::ExprStmt>( &Context.Ctx.Ast.Stmt( Id ) ) )
            {
                return Context.IsNoReturn( Context.Ctx.Values.ExprType( Stmt->Expr ) );
            }
            return false;
        }();

        if ( ( bDiverges or bExprDiverges ) and Index + 1 < Stmts.Size() )
        {
            const Frontend::StmtId NextStmt = Stmts[Index + 1];
            if ( NextStmt.IsValid() )
            {
                Context.Report( Frontend::LocOf( Context.Ctx.Ast.Stmt( NextStmt ) ),
                                "unreachable code after control flow interrupt" );
            }
            break;
        }
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

                // Definite assignment: a typed declaration with no initializer
                // (`x : T`) is uninitialized until explicitly assigned.
                if ( Bound.IsValid() and not Node.Init.IsValid() )
                {
                    Context.UninitializedLocals.insert( Node.Name );
                }
                else
                {
                    Context.UninitializedLocals.erase( Node.Name );
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
