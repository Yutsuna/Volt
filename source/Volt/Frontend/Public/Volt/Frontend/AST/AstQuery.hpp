#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <optional>
#include <string_view>
#include <variant>

namespace Volt
{

namespace Frontend
{

    // Small structural queries over an AstContext. They flatten the
    // `std::get_if` cascades that any AST *reader* (Driver manifest walk,
    // tooling, future checks) would otherwise re-write by hand: matching one
    // more shape stays a one-liner at the call site.

    /// The source range of any node, whatever its category. Every node
    /// struct opens with a `Loc` field by convention (the one field
    /// reflection skips), so this needs no per-kind switch and cannot fall
    /// behind the manifest. A valueless variant yields an invalid range.
    template <typename NodeVariant> [[nodiscard]] Core::SourceRange LocOf ( const NodeVariant &Node )
    {
        return std::visit(
            [] ( const auto &Concrete ) -> Core::SourceRange
            {
                if constexpr ( requires { Concrete.Loc; } )
                {
                    return Concrete.Loc;
                }
                else
                {
                    return Core::SourceRange{};
                }
            },
            Node );
    }

    /// The text of a StringLiteral expression, if that is what Id points at.
    /// The view lives as long as the context's interner.
    [[nodiscard]] inline std::optional<std::string_view> AsStringText ( const AstContext &Ast, ExprId Id )
    {
        if ( !Id.IsValid() )
        {
            return std::nullopt;
        }
        if ( const auto *Lit = std::get_if<StringLiteral>( &Ast.Expr( Id ) ) )
        {
            return Ast.Text( Lit->Value );
        }
        return std::nullopt;
    }

    /// The Call carried by an ExprStmt statement, if that is its shape.
    [[nodiscard]] inline const Call *StmtAsCall ( const AstContext &Ast, StmtId Id )
    {
        if ( !Id.IsValid() )
        {
            return nullptr;
        }
        const auto *Wrapper = std::get_if<ExprStmt>( &Ast.Stmt( Id ) );
        if ( Wrapper == nullptr )
        {
            return nullptr;
        }
        return std::get_if<Call>( &Ast.Expr( Wrapper->Expr ) );
    }

    /// The callee's name when a call is `name(...)` on a plain identifier.
    [[nodiscard]] inline std::optional<std::string_view> CalleeName ( const AstContext &Ast, const Call &Node )
    {
        if ( const auto *Callee = std::get_if<Identifier>( &Ast.Expr( Node.Callee ) ) )
        {
            return Ast.Text( Callee->Name );
        }
        return std::nullopt;
    }

    /// The Binary node behind Id when its operator is exactly Op (e.g. the
    /// `key => value` rows of a manifest call).
    [[nodiscard]] inline const Binary *AsBinaryOp ( const AstContext &Ast, ExprId Id, TokenKind Op )
    {
        if ( !Id.IsValid() )
        {
            return nullptr;
        }
        const auto *Node = std::get_if<Binary>( &Ast.Expr( Id ) );
        if ( Node == nullptr or Node->Op != Op )
        {
            return nullptr;
        }
        return Node;
    }

} // namespace Frontend

} // namespace Volt
