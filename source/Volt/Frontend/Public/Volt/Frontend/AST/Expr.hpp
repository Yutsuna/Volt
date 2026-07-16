#pragma once

#include "Volt/Frontend/AST/Jsx.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cstddef>
#include <string_view>
#include <variant>

namespace Volt
{

    namespace Frontend
    {

        // Literals keep the raw lexeme (interned); decoding to a typed value is
        // a later, zero-hardcode concern of Sema, not the parser.
        struct IntLiteral
        {

            using Self = IntLiteral;

            Core::SourceRange Loc;
            Symbol            Raw;

            VOLT_FIELDS( Raw )
        };

        struct FloatLiteral
        {

            using Self = FloatLiteral;

            Core::SourceRange Loc;
            Symbol            Raw;

            VOLT_FIELDS( Raw )
        };

        struct StringLiteral
        {

            using Self = StringLiteral;

            Core::SourceRange Loc;
            Symbol            Value;

            VOLT_FIELDS( Value )
        };

        struct CharLiteral
        {

            using Self = CharLiteral;

            Core::SourceRange Loc;
            Symbol            Raw;

            VOLT_FIELDS( Raw )
        };

        struct BoolLiteral
        {

            using Self = BoolLiteral;

            Core::SourceRange Loc;
            bool              Value = false;

            VOLT_FIELDS( Value )
        };

        struct NilLiteral
        {

            using Self = NilLiteral;

            Core::SourceRange Loc;

            VOLT_FIELDS_NONE()
        };

        struct SymbolLiteral
        {

            using Self = SymbolLiteral;

            Core::SourceRange Loc;
            Symbol            Name;

            VOLT_FIELDS( Name )
        };

        // `[a, b] of T` — ElemType is invalid when the `of T` is omitted.
        struct ArrayLit
        {

            using Self = ArrayLit;

            Core::SourceRange Loc;
            ExprList          Elements;
            TypeId            ElemType;

            VOLT_FIELDS( Elements, ElemType )
        };

        // `{ k => v }` — parallel key/value lists.
        struct HashLit
        {

            using Self = HashLit;

            Core::SourceRange Loc;
            ExprList          Keys;
            ExprList          Values;
            TypeId            KeyType;
            TypeId            ValueType;

            VOLT_FIELDS( Keys, Values, KeyType, ValueType )
        };

        struct Identifier
        {

            using Self = Identifier;

            Core::SourceRange Loc;
            Symbol            Name;

            VOLT_FIELDS( Name )
        };

        struct InstanceVar
        {

            using Self = InstanceVar;

            Core::SourceRange Loc;
            Symbol            Name;

            VOLT_FIELDS( Name )
        };

        struct SelfExpr
        {

            using Self = SelfExpr;

            Core::SourceRange Loc;

            VOLT_FIELDS_NONE()
        };

        struct Binary
        {

            using Self = Binary;

            Core::SourceRange Loc;
            TokenKind         Op = TokenKind::Error;
            ExprId            Lhs;
            ExprId            Rhs;

            VOLT_FIELDS( Op, Lhs, Rhs )
        };

        struct Unary
        {

            using Self = Unary;

            Core::SourceRange Loc;
            TokenKind         Op = TokenKind::Error;
            ExprId            Operand;

            VOLT_FIELDS( Op, Operand )
        };

        struct Ternary
        {

            using Self = Ternary;

            Core::SourceRange Loc;
            ExprId            Cond;
            ExprId            Then;
            ExprId            Else;

            VOLT_FIELDS( Cond, Then, Else )
        };

        // `Target Op= Value` — Op is `=` or a compound assignment token.
        struct Assign
        {

            using Self = Assign;

            Core::SourceRange Loc;
            TokenKind         Op = TokenKind::Assign;
            ExprId            Target;
            ExprId            Value;

            VOLT_FIELDS( Op, Target, Value )
        };

        // `Callee(Args...)`. ArgNames is parallel to Args; an invalid Symbol
        // marks a positional argument, a valid one a `name: value` argument.
        struct Call
        {

            using Self = Call;

            Core::SourceRange Loc;
            ExprId            Callee;
            ExprList          Args;
            SymbolList        ArgNames;

            VOLT_FIELDS( Callee, Args, ArgNames )
        };

        // `Object[Args...]`
        struct Index
        {

            using Self = Index;

            Core::SourceRange Loc;
            ExprId            Object;
            ExprList          Args;

            VOLT_FIELDS( Object, Args )
        };

        // `Object.Name` — the receiver side of a call or a bare field access.
        struct Member
        {

            using Self = Member;

            Core::SourceRange Loc;
            ExprId            Object;
            Symbol            Name;

            VOLT_FIELDS( Object, Name )
        };

        struct SizeOf
        {

            using Self = SizeOf;

            Core::SourceRange Loc;
            TypeId            Type;

            VOLT_FIELDS( Type )
        };

        // `*Operand` in value position.
        struct Deref
        {

            using Self = Deref;

            Core::SourceRange Loc;
            ExprId            Operand;

            VOLT_FIELDS( Operand )
        };

        // String interpolation: an alternating run of StringLiteral chunks and
        // embedded expressions, in source order.
        struct Interp
        {

            using Self = Interp;

            Core::SourceRange Loc;
            ExprList          Parts;

            VOLT_FIELDS( Parts )
        };

        enum class ExprKind
        {

            None,
#define VOLT_EXPR( Name ) Name,
#include "Volt/Frontend/AST/Nodes.inl"
        };

        using ExprNode = std::variant<std::monostate
#define VOLT_EXPR( Name ) , Name
#include "Volt/Frontend/AST/Nodes.inl"
                                      >;

        [[nodiscard]] constexpr ExprKind KindOf( const ExprNode& Node )
        {
            return static_cast<ExprKind>( Node.index() );
        }

        [[nodiscard]] constexpr std::string_view NodeName( const ExprNode& Node )
        {
            switch ( KindOf( Node ) )
            {
                case ExprKind::None:
                    return "None";
#define VOLT_EXPR( Name )                                                                                                         \
    case ExprKind::Name:                                                                                                          \
        return #Name;
#include "Volt/Frontend/AST/Nodes.inl"
            }
            return "?";
        }

    }

}
