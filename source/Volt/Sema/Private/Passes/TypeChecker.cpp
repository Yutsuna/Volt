// TypeChecker.cpp — Order 30 pass: gives every expression a type, and
// resolves the members that types make available.

#include "TypeChecker/DeclStmtWalker.hpp"
#include "TypeChecker/LiteralInferencer.hpp"
#include "TypeChecker/TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstddef>
#include <variant>

namespace
{

// `T?` parses, and then nothing at all happens to it: ResolveTypeExpr falls
// into its generic branch, finds no stdlib type claiming the `NilableType`
// node kind, and hands back an invalid type in silence. Every use of `T?` was
// therefore accepted and untyped.
//
// Nilable types need a sum-type model Volt does not have, so this is refused
// rather than implemented — but refused *out loud*. An arena sweep catches
// every occurrence with its own SourceRange, including the ones no
// declaration reaches.
void RejectNilableTypes ( Volt::Sema::PassContext &Context )
{
    const std::size_t Count = Context.Ast.TypeCount();
    for ( std::size_t Index = 0; Index < Count; ++Index )
    {
        const Volt::Frontend::TypeId Id{ static_cast<Volt::Frontend::TypeId::ValueType>( Index ) };
        if ( std::holds_alternative<Volt::Frontend::NilableType>( Context.Ast.Type( Id ) ) )
        {
            Context.Diags.Error( Volt::Frontend::LocOf( Context.Ast.Type( Id ) ), "nilable types are not implemented" );
        }
    }
}

} // namespace

void Volt::Sema::TypeChecker ( PassContext &Context )
{
    RejectNilableTypes( Context );

    TypeCheckerPass::TypeCheckerContext State{ Context, TypeCheckerPass::MetadataExprs( Context.Ast ) };
    TypeCheckerPass::WalkDecls( State, Context.Ast.TopDecls );

    for ( const Frontend::StmtId Id : Context.Ast.TopStmts )
    {
        TypeCheckerPass::WalkStmt( State, Id );
    }

    // Snapshot the settled resolutions into the unit before the pass-local
    // state dies — inference refines entries in place, so only the final map
    // is the truth a backend may read (Layout/CalleeMap.hpp).
    if ( Context.Callees != nullptr )
    {
        for ( const auto &[Value, Found] : State.CalleeResolution )
        {
            Context.Callees->Set( Frontend::ExprId{ Value }, CalleeEntry{ .Decl        = Found.Decl,
                                                                          .Result      = Found.Result,
                                                                          .Params      = Found.Params,
                                                                          .BlockParam  = Found.BlockParam,
                                                                          .Bindings    = Found.Bindings,
                                                                          .Receiver    = Found.Receiver,
                                                                          .bConstructs = Found.bConstructs } );
        }
    }
}
