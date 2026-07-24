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

    // --- Locating nodes a nested parse produced --------------------------
    //
    // Two front-end paths re-lex a fragment into an existing AstContext: the
    // interpolation sub-parser and macro expansion. Both hand the Lexer a
    // string that starts at offset zero, so every node they build claims to
    // sit at the head of the file — a diagnostic inside `#{...}` or a macro
    // body points at line 1, and `__LINE__` expands to 1 with it. Bracketing
    // the arenas around the nested parse gives the caller exactly the nodes
    // it created, to put back on the enclosing file's coordinates.

    /// Every arena's size at one instant. Nodes appended after it occupy the
    /// half-open range [Mark, Count) of their category.
    struct ArenaMark
    {

        std::size_t Exprs  = 0;
        std::size_t Stmts  = 0;
        std::size_t Decls  = 0;
        std::size_t Types  = 0;
        std::size_t Params = 0;
    };

    [[nodiscard]] inline ArenaMark MarkArenas ( const AstContext &Ast )
    {
        return ArenaMark{ .Exprs  = Ast.ExprCount(),
                          .Stmts  = Ast.StmtCount(),
                          .Decls  = Ast.DeclCount(),
                          .Types  = Ast.TypeCount(),
                          .Params = Ast.ParamCount() };
    }

    /// Invoke Fn( Core::SourceRange & ) on every node appended since Mark.
    /// Like LocOf, this leans on the `Loc` convention rather than a per-kind
    /// switch, so a new node category member is covered the day it is added.
    template <typename Fn> void ForEachLocSince ( AstContext &Ast, const ArenaMark &Mark, Fn &&Apply )
    {
        const auto Visit = [&Apply] ( auto &Concrete )
        {
            if constexpr ( requires { Concrete.Loc; } )
            {
                Apply( Concrete.Loc );
            }
        };

        for ( std::size_t Index = Mark.Exprs; Index < Ast.ExprCount(); ++Index )
        {
            std::visit( Visit, Ast.Expr( ExprId{ static_cast<ExprId::ValueType>( Index ) } ) );
        }
        for ( std::size_t Index = Mark.Stmts; Index < Ast.StmtCount(); ++Index )
        {
            std::visit( Visit, Ast.Stmt( StmtId{ static_cast<StmtId::ValueType>( Index ) } ) );
        }
        for ( std::size_t Index = Mark.Decls; Index < Ast.DeclCount(); ++Index )
        {
            std::visit( Visit, Ast.Decl( DeclId{ static_cast<DeclId::ValueType>( Index ) } ) );
        }
        for ( std::size_t Index = Mark.Types; Index < Ast.TypeCount(); ++Index )
        {
            std::visit( Visit, Ast.Type( TypeId{ static_cast<TypeId::ValueType>( Index ) } ) );
        }
        for ( std::size_t Index = Mark.Params; Index < Ast.ParamCount(); ++Index )
        {
            Visit( Ast.GetParam( ParamId{ static_cast<ParamId::ValueType>( Index ) } ) );
        }
    }

    /// Slide nodes appended since Mark by Offset. For a fragment that *is* a
    /// verbatim slice of the file (string interpolation), so offsets map back
    /// exactly and each node keeps its own distinct position.
    inline void ShiftLocsSince ( AstContext &Ast, const ArenaMark &Mark, std::uint32_t Offset )
    {
        ForEachLocSince( Ast, Mark,
                         [Offset] ( Core::SourceRange &Loc )
                         {
                             Loc.Begin += Offset;
                             Loc.End += Offset;
                         } );
    }

    /// Collapse nodes appended since Mark onto one range. For synthesised
    /// text (macro expansion), which has no per-character preimage in the
    /// file: the honest location for all of it is the invocation site.
    inline void StampLocsSince ( AstContext &Ast, const ArenaMark &Mark, Core::SourceRange Range )
    {
        ForEachLocSince( Ast, Mark, [Range] ( Core::SourceRange &Loc ) { Loc = Range; } );
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
