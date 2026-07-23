#include "LiteralInferencer.hpp"

void Volt::Sema::TypeCheckerPass::MarkMetadata ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::vector<bool> &Marked )
{
    if ( not Id.IsValid() or Id.Value >= Marked.size() or Marked[Id.Value] )
    {
        return;
    }
    Marked[Id.Value] = true;

    std::visit(
        [&] ( const auto &Concrete )
        {
            if constexpr ( Meta::Reflected<decltype( Concrete )> )
            {
                Meta::ForEachField( Concrete,
                                    [&] ( std::string_view, const auto &Field )
                                    {
                                        using FieldType = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                        {
                                            MarkMetadata( Ast, Field, Marked );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                MarkMetadata( Ast, Child, Marked );
                                            }
                                        }
                                    } );
            }
        },
        Ast.Expr( Id ) );
}

std::vector<bool> Volt::Sema::TypeCheckerPass::MetadataExprs ( const Frontend::AstContext &Ast )
{
    std::vector<bool> Marked( Ast.ExprCount(), false );

    const std::size_t Count = Ast.DeclCount();
    for ( std::size_t Index = 0; Index < Count; ++Index )
    {
        const Frontend::DeclId Id{ static_cast<std::uint32_t>( Index ) };
        if ( const auto *Anno = std::get_if<Frontend::Annotation>( &Ast.Decl( Id ) ) )
        {
            for ( const Frontend::ExprId Arg : Anno->Args )
            {
                MarkMetadata( Ast, Arg, Marked );
            }
        }
    }
    return Marked;
}
