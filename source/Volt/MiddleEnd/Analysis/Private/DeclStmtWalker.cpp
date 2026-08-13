#include "DeclStmtWalker.hpp"

#include "ClosureInferencer.hpp"
#include "MemberResolver.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeCompat.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <algorithm>
#include <vector>

namespace
{

using namespace Volt;
using namespace Volt::MiddleEnd::TypeSystem;
using namespace Volt::MiddleEnd::Resolver;
using namespace Volt::MiddleEnd::Core;

// Every mixin reachable through `include`, transitively and without
// repeats. The superclass is deliberately not followed: whatever it
// includes, it was itself required to implement.
void CollectIncludes ( const MiddleEnd::TypeSystem::TypeStore &Store,
                       MiddleEnd::TypeSystem::NominalId Id,
                       std::vector<MiddleEnd::TypeSystem::NominalId> &Out,
                       std::uint32_t Depth )
{
    if ( not Id.IsValid() or Depth > 16 )
    {
        return;
    }

    for ( const MiddleEnd::TypeSystem::SigTypeId Mixin : Store.Type( Id ).Includes )
    {
        const MiddleEnd::TypeSystem::NominalId Base = Store.BaseOf( Mixin );
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
void CheckAbstractConformance ( MiddleEnd::Analysis::TypeCheckerContext &Context,
                                MiddleEnd::TypeSystem::NominalId Id,
                                Volt::Core::SourceRange Loc )
{
    std::vector<MiddleEnd::TypeSystem::NominalId> Mixins;
    CollectIncludes( Context.Ctx.Types, Id, Mixins, 0 );

    for ( const MiddleEnd::TypeSystem::NominalId Mixin : Mixins )
    {
        for ( const MiddleEnd::TypeSystem::Member &Entry : Context.Ctx.Types.Type( Mixin ).Members )
        {
            if ( not Entry.bAbstract )
            {
                continue;
            }

            const std::string_view Name = Context.Ctx.Types.Text( Entry.Name );

            // On a primitive, `+` is a machine instruction, not a body the
            // stdlib could ever write. Same exemption as the unknown-member
            // diagnostic, through the same predicate.
            if ( MiddleEnd::Analysis::IsBuiltinOpOn( Context, Id, Name ) )
            {
                continue;
            }

            const MiddleEnd::TypeSystem::InstantiatedMember Found =
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

void Volt::MiddleEnd::Analysis::EnterType ( TypeCheckerContext &Context,
                                            NominalId Id,
                                            const Frontend::SymbolList &Params,
                                            const Frontend::DeclList &Body,
                                            Volt::Core::SourceRange Loc,
                                            bool bConcrete )
{
    const NominalId OuterType                 = Context.SelfType;
    const Frontend::SymbolList *OuterGenerics = Context.SelfGenerics;
    const SemaTypeId OuterValue               = Context.SelfValue;

    const bool bOuterGenericBody = Context.bGenericBody;

    Context.SelfType     = Id;
    Context.SelfGenerics = &Params;
    Context.bGenericBody = bOuterGenericBody or not Params.IsEmpty();

    Volt::Core::SmallVec<SemaTypeId, 2> Args;
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
    Context.bGenericBody = bOuterGenericBody;
}

void Volt::MiddleEnd::Analysis::EnterMethod ( TypeCheckerContext &Context, const Frontend::Method &Node )
{
    std::unordered_map<BindingSite, SemaTypeId, BindingSiteHash> OuterLocalTypes;
    OuterLocalTypes.swap( Context.LocalTypes );
    std::unordered_map<Symbol, BindingSite> OuterLocalSites;
    OuterLocalSites.swap( Context.LocalSites );
    std::unordered_map<Symbol, SemaTypeId> OuterLocals;
    OuterLocals.swap( Context.Locals );
    std::unordered_set<std::uint32_t> OuterUnconstrainedLiterals;
    OuterUnconstrainedLiterals.swap( Context.UnconstrainedLiterals );
    std::unordered_map<Symbol, Frontend::ExprId> OuterUnconstrainedVarInitializers;
    OuterUnconstrainedVarInitializers.swap( Context.UnconstrainedVarInitializers );
    std::unordered_set<Symbol> OuterUninitializedLocals;
    OuterUninitializedLocals.swap( Context.UninitializedLocals );
    const SemaTypeId OuterReturnType = Context.CurrentMethodReturnType;

    UnitSink Sink = Context.MakeSink();
    Context.CurrentMethodReturnType =
        ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Node.ReturnType );

    const bool bOuterStatic      = Context.bStaticContext;
    Context.bStaticContext       = Node.bSelf;
    const bool bOuterGenericBody = Context.bGenericBody;
    // A method's own generics (`map<U>`) are not in Context.Generics() — that
    // span is the enclosing type's — so `U` resolves to nothing exactly like a
    // type parameter does. Same deferral, same reason.
    Context.bGenericBody = bOuterGenericBody or not Node.Generics.IsEmpty();

    for ( const Frontend::ParamId Id : Node.Params )
    {
        const Frontend::Param &Entry = Context.Ctx.Ast.GetParam( Id );
        const SemaTypeId ParamType =
            ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Entry.DeclType );
        Context.LocalTypes[BindingSite{ Id }] = ParamType;
        Context.LocalSites[Entry.Name]        = BindingSite{ Id };
        Context.Locals[Entry.Name]            = ParamType;
        Context.Ctx.Values.SetSiteType( BindingSite{ Id }, ParamType );
        if ( Entry.Default.IsValid() and ParamType.IsValid() )
        {
            Context.ConstrainExprType( Entry.Default, ParamType );
        }
        InferExpr( Context, Entry.Default );
        if ( Entry.Default.IsValid() and ParamType.IsValid() )
        {
            Context.ConstrainExprType( Entry.Default, ParamType );
        }
        CheckAssignable( Context, Entry.Default, ParamType, EAssignSite::ParamDefault );
    }

    // TrailingType is WalkStmts plus "what did the last ExprStmt evaluate to",
    // which is the value a body-without-`return` produces. A `-> Void` method
    // has an invalid return type, so its trailing value is checked against
    // nothing — which is the correct behaviour, reached without naming a type.
    const SemaTypeId Trailing = TrailingType( Context, Node.Body );
    if ( Node.Body.Size() > 0 )
    {
        if ( const auto *Last = std::get_if<Frontend::ExprStmt>( &Context.Ctx.Ast.Stmt( Node.Body[Node.Body.Size() - 1] ) );
             Last != nullptr and Trailing.IsValid() )
        {
            Context.ConstrainExprType( Last->Expr, Context.CurrentMethodReturnType );
            CheckAssignable( Context, Last->Expr, Context.CurrentMethodReturnType, EAssignSite::Trailing );
        }
    }

    Context.bStaticContext          = bOuterStatic;
    Context.bGenericBody            = bOuterGenericBody;
    Context.CurrentMethodReturnType = OuterReturnType;
    Context.LocalTypes.swap( OuterLocalTypes );
    Context.LocalSites.swap( OuterLocalSites );
    Context.Locals.swap( OuterLocals );
    Context.UnconstrainedLiterals.swap( OuterUnconstrainedLiterals );
    Context.UnconstrainedVarInitializers.swap( OuterUnconstrainedVarInitializers );
    Context.UninitializedLocals.swap( OuterUninitializedLocals );
}

void Volt::MiddleEnd::Analysis::WalkDecl ( TypeCheckerContext &Context, Frontend::DeclId Id )
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

void Volt::MiddleEnd::Analysis::WalkStmts ( TypeCheckerContext &Context, const Frontend::StmtList &Stmts )
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

void Volt::MiddleEnd::Analysis::WalkStmt ( TypeCheckerContext &Context, Frontend::StmtId Id )
{
    if ( not Id.IsValid() )
    {
        return;
    }

    // Copied out, not visited by reference: a LocalDecl/Return/default
    // handler below reads several fields interleaved with InferExpr calls,
    // and — since LowerArrayLit — one of those calls can Add() onto the Stmt
    // arena, reallocating out from under a reference bound straight into the
    // live slot (rules/ast-rewrite.md). ComputeExpr takes the same copy for
    // the Expr arena for the identical reason.
    const Frontend::StmtNode StmtCopy = Context.Ctx.Ast.Stmt( Id );

    std::visit(
        Meta::Overloaded{
            [&] ( const Frontend::LocalDecl &Node )
            {
                UnitSink Sink = Context.MakeSink();
                const SemaTypeId Written =
                    ResolveTypeExpr( Context.Ctx.Ast, Context.Ctx.Types, Context.Generics(), Sink, Node.DeclType );
                if ( Written.IsValid() and Node.Init.IsValid() )
                {
                    Context.ConstrainExprType( Node.Init, Written );
                }
                const SemaTypeId Init = InferExpr( Context, Node.Init );

                // Constrained a second time, *after* inference: a literal only
                // enters UnconstrainedLiterals while being inferred, so the
                // call above — which runs before it, because that is what feeds
                // ExpectedClosure to a lambda initialiser — cannot narrow it.
                // Without this second pass `a : UInt64 = 8` reports Int32.
                if ( Written.IsValid() )
                {
                    Context.ConstrainExprType( Node.Init, Written );
                }
                CheckAssignable( Context, Node.Init, Written, EAssignSite::LocalDecl );

                const SemaTypeId Bound                = Written.IsValid() ? Written : Init;
                Context.LocalTypes[BindingSite{ Id }] = Bound;
                Context.LocalSites[Node.Name]         = BindingSite{ Id };
                Context.Locals[Node.Name]             = Bound;
                Context.Ctx.Values.SetSiteType( BindingSite{ Id }, Bound );
                if ( not Written.IsValid() and Node.Init.IsValid() and Context.IsUnconstrainedInit( Node.Init, Init ) )
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
                if ( Node.Value.IsValid() and Context.CurrentMethodReturnType.IsValid() )
                {
                    Context.ConstrainExprType( Node.Value, Context.CurrentMethodReturnType );
                }
                CheckAssignable( Context, Node.Value, Context.CurrentMethodReturnType, EAssignSite::Return );
            },
            [&] ( const auto &Node ) { WalkChildren( Context, Node ); },
        },
        StmtCopy );
}
