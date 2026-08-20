#include "LiteralInferencer.hpp"

#include <cstdint>
#include <type_traits>
#include <variant>

namespace
{

using Volt::Frontend::AstContext;

void MarkStmt ( const AstContext &Ast, Volt::Frontend::StmtId Id, std::vector<bool> &Marked );
void MarkDecl ( const AstContext &Ast, Volt::Frontend::DeclId Id, std::vector<bool> &Marked );

// One field dispatch for all three categories. Metadata is a property of a
// whole subtree, and a macro body is made of statements and nested
// declarations, not of expressions alone — walking expression fields only
// would leave the `if` inside a macro body unmarked, which is exactly the
// shape that used to reach AstInvariant as a false positive.
template <typename NodeType> void MarkFields ( const AstContext &Ast, const NodeType &Node, std::vector<bool> &Marked )
{
    if constexpr ( Volt::Meta::Reflected<NodeType> )
    {
        Volt::Meta::ForEachField(
            Node,
            [&] ( std::string_view, const auto &Field )
            {
                using FieldType = std::remove_cvref_t<decltype( Field )>;
                if constexpr ( std::is_same_v<FieldType, Volt::Frontend::ExprId> )
                {
                    Volt::MiddleEnd::Analysis::MarkMetadata( Ast, Field, Marked );
                }
                else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::ExprList> )
                {
                    for ( const Volt::Frontend::ExprId Child : Field )
                    {
                        Volt::MiddleEnd::Analysis::MarkMetadata( Ast, Child, Marked );
                    }
                }
                else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::StmtId> )
                {
                    MarkStmt( Ast, Field, Marked );
                }
                else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::StmtList> )
                {
                    for ( const Volt::Frontend::StmtId Child : Field )
                    {
                        MarkStmt( Ast, Child, Marked );
                    }
                }
                else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::DeclId> )
                {
                    MarkDecl( Ast, Field, Marked );
                }
                else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::DeclList> )
                {
                    for ( const Volt::Frontend::DeclId Child : Field )
                    {
                        MarkDecl( Ast, Child, Marked );
                    }
                }
                else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::ParamId> )
                {
                    Volt::MiddleEnd::Analysis::MarkMetadata( Ast, Ast.GetParam( Field ).Default, Marked );
                }
                else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::ParamList> )
                {
                    for ( const Volt::Frontend::ParamId Child : Field )
                    {
                        Volt::MiddleEnd::Analysis::MarkMetadata( Ast, Ast.GetParam( Child ).Default, Marked );
                    }
                }
            } );
    }
}

void MarkStmt ( const AstContext &Ast, Volt::Frontend::StmtId Id, std::vector<bool> &Marked )
{
    if ( not Id.IsValid() )
    {
        return;
    }
    std::visit( [&] ( const auto &Node ) { MarkFields( Ast, Node, Marked ); }, Ast.Stmt( Id ) );
}

void MarkDecl ( const AstContext &Ast, Volt::Frontend::DeclId Id, std::vector<bool> &Marked )
{
    if ( not Id.IsValid() )
    {
        return;
    }
    std::visit( [&] ( const auto &Node ) { MarkFields( Ast, Node, Marked ); }, Ast.Decl( Id ) );
}

} // namespace

void Volt::MiddleEnd::Analysis::MarkMetadata ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::vector<bool> &Marked )
{
    if ( not Id.IsValid() or Id.Value >= Marked.size() or Marked[Id.Value] )
    {
        return;
    }
    Marked[Id.Value] = true;

    std::visit( [&] ( const auto &Node ) { MarkFields( Ast, Node, Marked ); }, Ast.Expr( Id ) );
}

std::vector<bool> Volt::MiddleEnd::Analysis::MetadataExprs ( const Frontend::AstContext &Ast )
{
    std::vector<bool> Marked( Ast.ExprCount(), false );

    const std::size_t Count = Ast.DeclCount();
    for ( std::size_t Index = 0; Index < Count; ++Index )
    {
        const Frontend::DeclId Id{ static_cast<std::uint32_t>( Index ) };

        std::visit(
            [&] ( const auto &Node )
            {
                using NodeType = std::remove_cvref_t<decltype( Node )>;
                // An annotation's arguments are spellings, not values: nothing
                // ever evaluates them.
                //
                // A macro's body is the same status for a different reason. It
                // is a compile-time program, consumed by ExpandTypeMacros at
                // the interface seam; what it *emits* is cloned into the target
                // type and type-checked there, as ordinary code. The template
                // itself stays in the arena — arenas only ever grow — and no
                // pass after the seam evaluates it, so its sugar is never
                // lowered and its expressions never typed. Both facts are
                // legitimate, and this is what tells AstInvariant so.
                if constexpr ( std::is_same_v<NodeType, Frontend::Annotation> or
                               std::is_same_v<NodeType, Frontend::MacroDef> or
                               std::is_same_v<NodeType, Frontend::MacroBlock> )
                {
                    MarkFields( Ast, Node, Marked );
                }
            },
            Ast.Decl( Id ) );
    }
    return Marked;
}
