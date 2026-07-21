// TypeChecker.cpp — Order 30 pass: resolves every literal to the Volt type
// that wraps it.
//
// In Volt there are no bare machine values: `10` is an Int32, `"x"` a String,
// `[ 1, 2 ]` an Array. Which type that is comes entirely from the stdlib's
// `@[Literal( ... )]` annotations, bound before this pass runs. The name
// "Int32" appears nowhere here — only the literal *kind*, which is an AST
// property, and whatever type claimed it.

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Sema/Pass.hpp"

#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace Volt
{

namespace Sema
{

    namespace
    {

        // The literal kind an expression is, or empty when it is not a
        // literal. The name matches the AST node's, so a new literal node in
        // Nodes.inl needs one line here and a `@[Literal]` in the stdlib —
        // never a change to the resolution logic below.
        [[nodiscard]] std::string_view LiteralKindOf ( const Frontend::ExprNode &Node )
        {
            return std::visit(
                Meta::Overloaded{
                    [] ( const Frontend::IntLiteral & ) { return std::string_view{ "IntLiteral" }; },
                    [] ( const Frontend::FloatLiteral & ) { return std::string_view{ "FloatLiteral" }; },
                    [] ( const Frontend::StringLiteral & ) { return std::string_view{ "StringLiteral" }; },
                    [] ( const Frontend::CharLiteral & ) { return std::string_view{ "CharLiteral" }; },
                    [] ( const Frontend::BoolLiteral & ) { return std::string_view{ "BoolLiteral" }; },
                    [] ( const Frontend::SymbolLiteral & ) { return std::string_view{ "SymbolLiteral" }; },
                    [] ( const Frontend::ArrayLit & ) { return std::string_view{ "ArrayLiteral" }; },
                    [] ( const Frontend::HashLit & ) { return std::string_view{ "HashLiteral" }; },
                    [] ( const auto & ) { return std::string_view{}; },
                },
                Node );
        }

        // Annotation arguments are compile-time metadata, not program values:
        // the "libc" in `@[External( "libc" )]` names a library, it is not a
        // Volt String. Since the pass scans the expression arena flat, it
        // cannot see that context — so mark those subtrees and skip them.
        //
        // Reflection walks the children, so a new node with expression fields
        // is covered without touching this pass.
        void MarkMetadata ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::vector<bool> &Marked )
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

        [[nodiscard]] std::vector<bool> MetadataExprs ( const Frontend::AstContext &Ast )
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

    } // namespace

    void TypeChecker ( PassContext &Context )
    {
        const std::vector<bool> Metadata = MetadataExprs( Context.Ast );

        const std::size_t Count = Context.Ast.ExprCount();
        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            if ( Metadata[Index] )
            {
                continue;
            }

            const Frontend::ExprId Id{ static_cast<std::uint32_t>( Index ) };
            const Frontend::ExprNode &Node = Context.Ast.Expr( Id );

            const std::string_view Kind = LiteralKindOf( Node );
            if ( Kind.empty() )
            {
                continue;
            }

            // A literal with no type to wrap it means the stdlib never
            // claimed that kind — a missing `@[Literal]`, not a user error
            // in the file being checked.
            if ( not Context.Types.LookupLiteral( Kind ) )
            {
                Context.Diags.Report( Core::Diagnostic{ .Severity = Core::ESeverity::Error,
                                                        .Range    = Frontend::LocOf( Node ),
                                                        .Message  = "no type claims " + std::string{ Kind } +
                                                                   "; the standard library must declare one with @[Literal( " +
                                                                   std::string{ Kind } + " )]",
                                                        .Notes = {} } );
            }
        }
    }

} // namespace Sema

} // namespace Volt
