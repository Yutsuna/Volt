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

        Core::SourceRange Loc{};
        ExprId Expr{};
    };

    // `while Cond ... end`, and `until` after the parser has negated its
    // condition. bPostTest marks the do-while form — `begin ... end while c`
    // — whose body runs once before Cond is ever evaluated. A flag rather
    // than a desugaring so that `next` still branches to the test and not to
    // the top of the body.
    struct While
    {

        Core::SourceRange Loc{};
        ExprId Cond{};
        StmtList Body{};
        bool bPostTest{};
    };

    // `return [Value]` — Value is invalid for a bare `return`.
    struct Return
    {

        Core::SourceRange Loc{};
        ExprId Value{};
    };

    // `break [Value]` — Value is invalid for a bare `break`.
    struct Break
    {

        Core::SourceRange Loc{};
        ExprId Value{};
    };

    // `next [Value]` — Value is invalid for a bare `next`.
    struct Next
    {

        Core::SourceRange Loc{};
        ExprId Value{};
    };

    // `Name : DeclType = Init` — DeclType and/or Init may be invalid.
    struct LocalDecl
    {

        Core::SourceRange Loc{};
        Symbol Name{};
        TypeId DeclType{};
        ExprId Init{};
    };

    // `when Pattern1, Pattern2 then Body...`
    struct WhenClause
    {

        Core::SourceRange Loc{};
        ExprList Patterns{};
        StmtList Body{};
    };

    // `rescue [VarName] [: ExceptionType] Body...`
    struct RescueClause
    {
        using Self = RescueClause;
        Core::SourceRange Loc{};
        Symbol VarName{};
        TypeId ExceptionType{};
        StmtList Body{};
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
