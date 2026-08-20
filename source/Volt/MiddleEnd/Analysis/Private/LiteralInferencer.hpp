#pragma once

#include "DeclStmtWalker.hpp"
#include "MemberResolver.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "VoltMiddleEndAnalysis_export.hpp"

#if VOLT_HAS_REFLECTION
    #include <meta>
#endif
#include <type_traits>
#include <vector>

namespace Volt::MiddleEnd::Analysis
{

template <typename NodeType> [[nodiscard]] consteval bool HasChildNodes ()
{
#if VOLT_HAS_REFLECTION
    for ( const auto Field : std::meta::nonstatic_data_members_of( ^^NodeType, std::meta::access_context::unchecked() ) )
    {
        const auto FieldType = std::meta::dealias( std::meta::type_of( Field ) );
        if ( FieldType == std::meta::dealias( ^^Frontend::ExprId ) or FieldType == std::meta::dealias( ^^Frontend::ExprList ) or
             FieldType == std::meta::dealias( ^^Frontend::TypeId ) or FieldType == std::meta::dealias( ^^Frontend::TypeList ) or
             FieldType == std::meta::dealias( ^^Frontend::StmtList ) or FieldType == std::meta::dealias( ^^Frontend::DeclList ) or
             FieldType == std::meta::dealias( ^^Frontend::ParamList ) )
        {
            return true;
        }
    }
    return false;
#else
    return false;
#endif
}

template <typename FieldType> void JoinField ( TypeCheckerContext &Context, SemaTypeId &Slot, const FieldType &Field )
{
    if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
    {
        if ( not Slot.IsValid() )
        {
            Slot = InferExpr( Context, Field );
        }
    }
    else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
    {
        for ( const Frontend::ExprId Child : Field )
        {
            if ( not Slot.IsValid() )
            {
                Slot = InferExpr( Context, Child );
            }
        }
    }
}

template <typename NodeType>
[[nodiscard]] Volt::Core::SmallVec<SemaTypeId, 2> SlotTypes ( TypeCheckerContext &Context, NominalId Base, const NodeType &Node )
{
    Volt::Core::SmallVec<SemaTypeId, 2> Args;
    const auto &Slots = Context.Ctx.Types.Type( Base ).LiteralSlots;

    if ( Slots.empty() )
    {
        Meta::ForEachField( Node,
                            [&] ( std::string_view, const auto &Field )
                            {
                                using FieldType = std::remove_cvref_t<decltype( Field )>;
                                if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> or
                                               std::is_same_v<FieldType, Frontend::ExprList> )
                                {
                                    SemaTypeId Slot;
                                    JoinField( Context, Slot, Field );
                                    Args.PushBack( Slot );
                                }
                            } );
        return Args;
    }

    for ( const auto &Names : Slots )
    {
        SemaTypeId Slot;
        Meta::ForEachField( Node,
                            [&] ( std::string_view FieldName, const auto &Field )
                            {
                                for ( const Symbol Wanted : Names )
                                {
                                    if ( Context.Ctx.Types.Text( Wanted ) == FieldName )
                                    {
                                        JoinField( Context, Slot, Field );
                                    }
                                }
                            } );
        Args.PushBack( Slot );
    }
    return Args;
}

template <typename NodeType>
[[nodiscard]] SemaTypeId LiteralType ( TypeCheckerContext &Context, Frontend::ExprId Id, const NodeType &Node )
{
    WalkChildren( Context, Node );

    if constexpr ( not Meta::Reflected<NodeType> )
    {
        return SemaTypeId{};
    }
    else
    {
        const std::string_view Kind = Meta::TypeName<NodeType>();
        if ( Kind == "IntLiteral" or Kind == "FloatLiteral" )
        {
            Context.UnconstrainedLiterals.insert( Id.Value );
        }
        const auto Base = Context.Ctx.Types.LookupNodeKind( Kind );
        if ( not Base )
        {
            if constexpr ( Meta::FieldCount<NodeType>() > 0 and not HasChildNodes<NodeType>() )
            {
                Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ),
                                "no type claims " + std::string{ Kind } +
                                    "; the standard library must declare one with @[Literal( " + std::string{ Kind } + " )]" );
            }
            return SemaTypeId{};
        }

        Volt::Core::SmallVec<SemaTypeId, 2> Args = SlotTypes( Context, *Base, Node );
        CheckArity( Context, Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) ), *Base, Args.Size() );
        return Context.MakeType( *Base, std::move( Args ) );
    }
}

} // namespace Volt::MiddleEnd::Analysis
