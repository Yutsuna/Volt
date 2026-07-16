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

            using Self = ExprStmt;

            Core::SourceRange Loc;
            ExprId            Expr;

            VOLT_FIELDS( Expr )
        };

        // `if Cond ... [else ...] end`. An `elsif` chain is represented as a
        // nested If living in the Else branch — no separate node needed.
        struct If
        {

            using Self = If;

            Core::SourceRange Loc;
            ExprId            Cond;
            StmtList          Then;
            StmtList          Else;

            VOLT_FIELDS( Cond, Then, Else )
        };

        struct While
        {

            using Self = While;

            Core::SourceRange Loc;
            ExprId            Cond;
            StmtList          Body;

            VOLT_FIELDS( Cond, Body )
        };

        // `return [Value]` — Value is invalid for a bare `return`.
        struct Return
        {

            using Self = Return;

            Core::SourceRange Loc;
            ExprId            Value;

            VOLT_FIELDS( Value )
        };

        // `Name : DeclType = Init` — DeclType and/or Init may be invalid.
        struct LocalDecl
        {

            using Self = LocalDecl;

            Core::SourceRange Loc;
            Symbol            Name;
            TypeId            DeclType;
            ExprId            Init;

            VOLT_FIELDS( Name, DeclType, Init )
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

        [[nodiscard]] constexpr StmtKind KindOf( const StmtNode& Node )
        {
            return static_cast<StmtKind>( Node.index() );
        }

        [[nodiscard]] constexpr std::string_view NodeName( const StmtNode& Node )
        {
            switch ( KindOf( Node ) )
            {
                case StmtKind::None:
                    return "None";
#define VOLT_STMT( Name )                                                                                                         \
    case StmtKind::Name:                                                                                                          \
        return #Name;
#include "Volt/Frontend/AST/Nodes.inl"
            }
            return "?";
        }

    }

}
