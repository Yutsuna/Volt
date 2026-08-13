#pragma once

#include "ExprInferencer.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"

namespace Volt::MiddleEnd::Analysis
{

// `bConcrete` says whether this declaration owes its mixins an
// implementation: a struct or a class does, a mixin does not — it is free
// to pass a contract along to whoever includes it.
void EnterType ( TypeCheckerContext &Context,
                 NominalId Id,
                 const Frontend::SymbolList &Params,
                 const Frontend::DeclList &Body,
                 Volt::Core::SourceRange Loc,
                 bool bConcrete );

void EnterMethod ( TypeCheckerContext &Context, const Frontend::Method &Node );

void WalkDecl ( TypeCheckerContext &Context, Frontend::DeclId Id );

template <typename DeclContainer> void WalkDecls ( TypeCheckerContext &Context, const DeclContainer &Decls )
{
    for ( const Frontend::DeclId Id : Decls )
    {
        WalkDecl( Context, Id );
    }
}

void WalkStmt ( TypeCheckerContext &Context, Frontend::StmtId Id );

void WalkStmts ( TypeCheckerContext &Context, const Frontend::StmtList &Stmts );

template <typename NodeType> void WalkChildren ( TypeCheckerContext &Context, const NodeType &Node )
{
    if constexpr ( Meta::Reflected<NodeType> )
    {
        Meta::ForEachField( Node,
                            [&] ( std::string_view, const auto &Field )
                            {
                                using FieldType = std::remove_cvref_t<decltype( Field )>;
                                if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                {
                                    static_cast<void>( InferExpr( Context, Field ) );
                                }
                                else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                {
                                    for ( const Frontend::ExprId Child : Field )
                                    {
                                        static_cast<void>( InferExpr( Context, Child ) );
                                    }
                                }
                                else if constexpr ( std::is_same_v<FieldType, Frontend::StmtList> )
                                {
                                    WalkStmts( Context, Field );
                                }
                                else if constexpr ( std::is_same_v<FieldType, Frontend::DeclList> )
                                {
                                    WalkDecls( Context, Field );
                                }
                                else if constexpr ( std::is_same_v<FieldType, Frontend::ParamList> )
                                {
                                    for ( const Frontend::ParamId Child : Field )
                                    {
                                        static_cast<void>( InferExpr( Context, Context.Ctx.Ast.GetParam( Child ).Default ) );
                                    }
                                }
                            } );
    }
}

} // namespace Volt::MiddleEnd::Analysis
