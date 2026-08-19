#pragma once

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace Volt::Frontend
{

/// Deep-clone an expression subtree from `Src` AstContext into `Dst` AstContext.
/// Symbols are re-interned into Dst's StringInterner.
inline ExprId CloneExpr ( const AstContext &Src, AstContext &Dst, ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return ExprId{};
    }

    const ExprNode &SrcNode = Src.Expr( Id );

    const auto CloneAlt = [&] ( const auto &Alt ) -> ExprNode
    {
        using AltType = std::remove_cvref_t<decltype( Alt )>;
        AltType Copy  = Alt;
        Meta::ForEachField( Copy,
                            [&] ( std::string_view, auto &Field )
                            {
                                using FieldType = std::remove_cvref_t<decltype( Field )>;
                                if constexpr ( std::same_as<FieldType, ExprId> )
                                {
                                    Field = CloneExpr( Src, Dst, Field );
                                }
                                else if constexpr ( std::same_as<FieldType, ExprList> or
                                                    std::same_as<FieldType, Core::SmallVec<ExprId, 2>> or
                                                    std::same_as<FieldType, Core::SmallVec<ExprId, 4>> )
                                {
                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                    {
                                        Field[Index] = CloneExpr( Src, Dst, Field[Index] );
                                    }
                                }
                                else if constexpr ( std::same_as<FieldType, Symbol> )
                                {
                                    Field = Dst.Strings().Intern( Src.Text( Field ) );
                                }
                                else if constexpr ( std::same_as<FieldType, SymbolList> )
                                {
                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                    {
                                        Field[Index] = Dst.Strings().Intern( Src.Text( Field[Index] ) );
                                    }
                                }
                            } );
        return ExprNode{ std::move( Copy ) };
    };

    ExprNode Cloned = std::visit( CloneAlt, SrcNode );
    return Dst.Add( std::move( Cloned ) );
}

} // namespace Volt::Frontend
