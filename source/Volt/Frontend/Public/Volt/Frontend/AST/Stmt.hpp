#pragma once

#include "Volt/Frontend/AST/Node.hpp"

#include <cstddef>
#include <string_view>
#include <variant>

namespace Volt
{

namespace Frontend
{

    struct ExprStmt
    {

        Core::SourceRange Loc;
        ExprId Expr;
    };

    // `if Cond ... [else ...] end`. An `elsif` chain is represented as a
    // nested If living in the Else branch — no separate node needed.
    struct If
    {

        Core::SourceRange Loc;
        ExprId Cond;
        StmtList Then;
        StmtList Else;
    };

    struct While
    {

        Core::SourceRange Loc;
        ExprId Cond;
        StmtList Body;
    };

    // `return [Value]` — Value is invalid for a bare `return`.
    struct Return
    {

        Core::SourceRange Loc;
        ExprId Value;
    };

    // `Name : DeclType = Init` — DeclType and/or Init may be invalid.
    struct LocalDecl
    {

        Core::SourceRange Loc;
        Symbol Name;
        TypeId DeclType;
        ExprId Init;
    };

    enum class StmtKind
    {

        None,
#define VOLT_STMT( Name ) Name,
#include "Volt/Frontend/AST/Nodes.inl"
    };

    using StmtNode = std::variant<std::monostate
#define VOLT_STMT( Name ) , Name
#include "Volt/Frontend/AST/Nodes.inl"
                                  >;

    [[nodiscard]] constexpr StmtKind KindOf ( const StmtNode &Node )
    {
        return static_cast<StmtKind>( Node.index() );
    }

} // namespace Frontend

} // namespace Volt
