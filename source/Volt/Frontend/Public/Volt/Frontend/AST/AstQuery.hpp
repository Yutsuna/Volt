#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

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

    // --- Metadata: the expressions nothing ever evaluates ----------------
    //
    // An annotation's arguments are *spellings*, and a macro body is a
    // compile-time program consumed at the interface seam. Neither is ever
    // lowered or typed, so both would show up as unlowered sugar or as untyped
    // values to any sweep that reads the arena by index rather than by walking
    // what the program actually runs.
    //
    // One mask, several consumers, and they must agree or a node would be
    // metadata to one and residue to another: the type checker skips these, the
    // AstInvariant census excludes them, and ConstEval's fold sweep must not
    // *execute* one — a command literal inside a macro body has already been
    // run, once, by the evaluator that consumed the macro.

    namespace Detail
    {
        inline void MarkMetadataStmt ( const AstContext &Ast, StmtId Id, std::vector<bool> &Marked );
        inline void MarkMetadataDecl ( const AstContext &Ast, DeclId Id, std::vector<bool> &Marked );

        /// One field dispatch for all three categories. Metadata is a property
        /// of a whole subtree, and a macro body is made of statements and
        /// nested declarations, not of expressions alone — walking expression
        /// fields only would leave the `if` inside a macro body unmarked.
        template <typename NodeType>
        void MarkMetadataFields ( const AstContext &Ast, const NodeType &Node, std::vector<bool> &Marked );
    } // namespace Detail

    /// Mark Id and everything below it as metadata.
    inline void MarkMetadata ( const AstContext &Ast, ExprId Id, std::vector<bool> &Marked )
    {
        if ( not Id.IsValid() or Id.Value >= Marked.size() or Marked[Id.Value] )
        {
            return;
        }
        Marked[Id.Value] = true;
        std::visit( [&] ( const auto &Node ) { Detail::MarkMetadataFields( Ast, Node, Marked ); }, Ast.Expr( Id ) );
    }

    namespace Detail
    {
        inline void MarkMetadataStmt ( const AstContext &Ast, StmtId Id, std::vector<bool> &Marked )
        {
            if ( not Id.IsValid() )
            {
                return;
            }
            std::visit( [&] ( const auto &Node ) { MarkMetadataFields( Ast, Node, Marked ); }, Ast.Stmt( Id ) );
        }

        inline void MarkMetadataDecl ( const AstContext &Ast, DeclId Id, std::vector<bool> &Marked )
        {
            if ( not Id.IsValid() )
            {
                return;
            }
            std::visit( [&] ( const auto &Node ) { MarkMetadataFields( Ast, Node, Marked ); }, Ast.Decl( Id ) );
        }

        template <typename NodeType>
        void MarkMetadataFields ( const AstContext &Ast, const NodeType &Node, std::vector<bool> &Marked )
        {
            if constexpr ( Meta::Reflected<NodeType> )
            {
                Meta::ForEachField( Node,
                                    [&] ( std::string_view, const auto &Field )
                                    {
                                        using FieldType = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<FieldType, ExprId> )
                                        {
                                            MarkMetadata( Ast, Field, Marked );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, ExprList> )
                                        {
                                            for ( const ExprId Child : Field )
                                            {
                                                MarkMetadata( Ast, Child, Marked );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, StmtId> )
                                        {
                                            MarkMetadataStmt( Ast, Field, Marked );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, StmtList> )
                                        {
                                            for ( const StmtId Child : Field )
                                            {
                                                MarkMetadataStmt( Ast, Child, Marked );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, DeclId> )
                                        {
                                            MarkMetadataDecl( Ast, Field, Marked );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, DeclList> )
                                        {
                                            for ( const DeclId Child : Field )
                                            {
                                                MarkMetadataDecl( Ast, Child, Marked );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, ParamId> )
                                        {
                                            MarkMetadata( Ast, Ast.GetParam( Field ).Default, Marked );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, ParamList> )
                                        {
                                            for ( const ParamId Child : Field )
                                            {
                                                MarkMetadata( Ast, Ast.GetParam( Child ).Default, Marked );
                                            }
                                        }
                                    } );
            }
        }
    } // namespace Detail

    /// Every expression the program never evaluates, by arena index. Read off
    /// the *Decl arena*, not the declaration lists: a macro is retired from its
    /// type's body once expanded (ConstEval::ExpandTypeMacros), and its
    /// template stays in the arena — where this still has to reach it.
    [[nodiscard]] inline std::vector<bool> MetadataExprs ( const AstContext &Ast )
    {
        std::vector<bool> Marked( Ast.ExprCount(), false );

        for ( std::size_t Index = 0; Index < Ast.DeclCount(); ++Index )
        {
            const DeclId Id{ static_cast<DeclId::ValueType>( Index ) };
            std::visit(
                [&] ( const auto &Node )
                {
                    using NodeType = std::remove_cvref_t<decltype( Node )>;
                    if constexpr ( std::is_same_v<NodeType, Annotation> or std::is_same_v<NodeType, MacroDef> or
                                   std::is_same_v<NodeType, MacroBlock> )
                    {
                        Detail::MarkMetadataFields( Ast, Node, Marked );
                    }
                },
                Ast.Decl( Id ) );
        }
        return Marked;
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
