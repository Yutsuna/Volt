// Reinstantiate.cpp — Sema::ReinstantiateBody (Instantiate.hpp), the one
// semantic step a monomorphising backend drives from outside a per-unit pass
// run.
//
// This is the *same* machinery TypeChecker itself uses — TrailingType,
// InferExpr, ConstrainExprType — just invoked a second time, over a fresh
// UnitTypes/UnitCallees rather than the unit's own, and with the generic
// body's parameters bound from the already-resolved Member::Params (via the
// public Instantiate()) instead of re-derived from written syntax through
// ResolveTypeExpr/UnitSink, which structurally cannot carry a binding
// (UnitSink::Param always refuses — the parameter case exists for ordinary,
// concrete method bodies, where a written `T` can never appear). Once self
// and the parameters carry concrete SemaTypeIds, every expression built on
// them — calls, operators, field access — resolves through the exact same
// LookupMemberOn / Instantiate / UnifySig this file's callers already trust
// for a non-generic body, because it is the same code.

#include "Volt/Sema/Layout/Instantiate.hpp"

#include "ClosureInferencer.hpp"
#include "LiteralInferencer.hpp"
#include "TypeCheckerContext.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"
#include "Volt/Sema/Pass.hpp"

#include <variant>

namespace
{

using namespace Volt;
using namespace Volt::Sema;

// Consume one MonoRequest-encoded subtree from the front of `Cursor` and
// intern it into `Values`: the same pre-order (NominalId, ArgCount, args...)
// encoding TypeMapper::FlattenValueType writes and BackendCore::ArgSubtree
// reads, decoded here rather than shared with it — Sema must not depend on
// Backend, which owns that side of the currency.
[[nodiscard]] SemaTypeId InternNext ( UnitTypes &Values, std::span<const std::uint32_t> &Cursor )
{
    if ( Cursor.size() < 2 )
    {
        Cursor = {};
        return SemaTypeId{};
    }

    const NominalId Base{ Cursor[0] };
    const std::uint32_t Count = Cursor[1];
    Cursor                    = Cursor.subspan( 2 );

    Core::SmallVec<SemaTypeId, 2> Args;
    Args.Reserve( Count );
    for ( std::uint32_t Index = 0; Index < Count; ++Index )
    {
        Args.PushBack( InternNext( Values, Cursor ) );
    }
    return Values.Intern( SemaType{ .Base = Base, .Args = std::move( Args ) } );
}

} // namespace

Volt::Sema::InstantiatedBody Volt::Sema::ReinstantiateBody ( const TypeStore &Store,
                                                             const Frontend::AstContext &Ast,
                                                             const ScopeTable &Scopes,
                                                             const Member &Entry,
                                                             NominalId Owner,
                                                             std::span<const std::uint32_t> FlatArgs )
{
    InstantiatedBody Result;

    // Decode the flattened bindings into fresh SemaTypeIds inside the
    // overlay: NominalId is the cross-unit, instantiation-independent
    // currency, so no caller-side SemaTypeId ever needs to cross into this
    // arena — everything concrete this function needs is built here.
    Core::SmallVec<SemaTypeId, 2> ReceiverArgs;
    std::span<const std::uint32_t> Cursor = FlatArgs;
    while ( not Cursor.empty() )
    {
        ReceiverArgs.PushBack( InternNext( Result.Values, Cursor ) );
    }

    const std::size_t OwnerGenericCount = Owner.IsValid() ? Store.Type( Owner ).Params.Size() : 0;
    Core::SmallVec<SemaTypeId, 2> OwnerArgs;
    for ( std::size_t Index = 0; Index < OwnerGenericCount and Index < ReceiverArgs.Size(); ++Index )
    {
        OwnerArgs.PushBack( ReceiverArgs[Index] );
    }
    const SemaTypeId Self =
        Owner.IsValid() ? Result.Values.Intern( SemaType{ .Base = Owner, .Args = std::move( OwnerArgs ) } ) : SemaTypeId{};

    // A scratch pass run: Ast/Scopes are read-only for an Analysis pass like
    // TypeChecker (only a Lowering pass rewrites them), so borrowing the
    // declaring unit's own is sound even though PassContext's fields are
    // mutable references — the same contract DefineMember already leans on
    // when it reads a UnitView's Ast/Scopes to emit a concrete body.
    Core::DiagEngine::Bag ScratchDiags;
    PassStats ScratchStats;
    PassContext ScratchCtx{
        // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast) — PassContext's
        // Ast/Scopes are mutable references for the sake of a Lowering pass;
        // TypeChecker (Analysis) never writes through either, so borrowing
        // the declaring unit's own read-only instances is sound.
        .Ast    = const_cast<Frontend::AstContext &>( Ast ),
        .Types  = Store,
        .Values = Result.Values,
        .Scopes = const_cast<ScopeTable &>( Scopes ),
        // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
        .Diags   = ScratchDiags,
        .Stats   = ScratchStats,
        .Globals = nullptr,
        .Sources = nullptr,
        .Callees = &Result.Callees,
    };

    TypeCheckerPass::TypeCheckerContext Context{ ScratchCtx, TypeCheckerPass::MetadataExprs( Ast ) };
    Context.SelfType  = Owner;
    Context.SelfValue = Self;
    // bGenericBody stays false (the default): every binding above is now
    // concrete, so nothing walked from here should defer — a node that still
    // cannot resolve is a genuine middle-end gap, not this instantiation's.

    const auto *MethodNode = std::get_if<Frontend::Method>( &Ast.Decl( Entry.Decl ) );
    if ( MethodNode == nullptr )
    {
        return Result;
    }

    std::size_t Index = 0;
    for ( const Frontend::ParamId ParamRef : MethodNode->Params )
    {
        if ( Index >= Entry.Params.Size() )
        {
            break;
        }
        const SemaTypeId ParamType = Instantiate( Store, Entry.Params[Index], ReceiverArgs, Self, Result.Values );
        const BindingSite Site{ ParamRef };
        Context.LocalTypes[Site]                      = ParamType;
        Context.Locals[Ast.GetParam( ParamRef ).Name] = ParamType;
        Result.Values.SetSiteType( Site, ParamType );
        ++Index;
    }
    Context.CurrentMethodReturnType = Instantiate( Store, Entry.Result, ReceiverArgs, Self, Result.Values );

    const SemaTypeId Trailing = TypeCheckerPass::TrailingType( Context, MethodNode->Body );
    if ( MethodNode->Body.Size() > 0 )
    {
        if ( const auto *Last = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( MethodNode->Body[MethodNode->Body.Size() - 1] ) );
             Last != nullptr and Trailing.IsValid() )
        {
            Context.ConstrainExprType( Last->Expr, Context.CurrentMethodReturnType );
        }
    }

    // Snapshot the resolutions this walk collected — the same final step
    // TypeChecker itself takes at the end of a unit's pass run.
    for ( const auto &[Value, Found] : Context.CalleeResolution )
    {
        Result.Callees.Set( Frontend::ExprId{ Value }, CalleeEntry{ .Decl       = Found.Decl,
                                                                    .Result     = Found.Result,
                                                                    .Params     = Found.Params,
                                                                    .BlockParam = Found.BlockParam,
                                                                    .Bindings   = Found.Bindings,
                                                                    .Receiver   = Found.Receiver } );
    }

    return Result;
}
