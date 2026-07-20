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

        Core::SourceRange Loc;
        Symbol Raw;
    };

    struct FloatLiteral
    {

        Core::SourceRange Loc;
        Symbol Raw;
    };

    struct StringLiteral
    {

        Core::SourceRange Loc;
        Symbol Value;
    };

    struct CharLiteral
    {

        Core::SourceRange Loc;
        Symbol Raw;
    };

    struct BoolLiteral
    {

        Core::SourceRange Loc;
        bool Value = false;
    };

    struct NilLiteral
    {

        Core::SourceRange Loc;
    };

    struct SymbolLiteral
    {

        Core::SourceRange Loc;
        Symbol Name;
    };

    // `[a, b] of T` — ElemType is invalid when the `of T` is omitted.
    struct ArrayLit
    {

        Core::SourceRange Loc;
        ExprList Elements;
        TypeId ElemType;
    };

    // `{ k => v }` — parallel key/value lists.
    struct HashLit
    {

        Core::SourceRange Loc;
        ExprList Keys;
        ExprList Values;
        TypeId KeyType;
        TypeId ValueType;
    };

    struct Identifier
    {

        Core::SourceRange Loc;
        Symbol Name;
    };

    struct InstanceVar
    {

        Core::SourceRange Loc;
        Symbol Name;
    };

    struct SelfExpr
    {

        Core::SourceRange Loc;
    };

    struct Binary
    {

        Core::SourceRange Loc;
        TokenKind Op = TokenKind::Error;
        ExprId Lhs;
        ExprId Rhs;
    };

    struct Unary
    {

        Core::SourceRange Loc;
        TokenKind Op = TokenKind::Error;
        ExprId Operand;
    };

    struct Ternary
    {

        Core::SourceRange Loc;
        ExprId Cond;
        ExprId Then;
        ExprId Else;
    };

    // `Target Op= Value` — Op is `=` or a compound assignment token.
    struct Assign
    {

        Core::SourceRange Loc;
        TokenKind Op = TokenKind::Assign;
        ExprId Target;
        ExprId Value;
    };

    // `do |Params| Body end` — a trailing block literal attached to a Call.
    struct Block
    {

        Core::SourceRange Loc;
        ParamList Params;
        StmtList Body;
    };

    // `Callee(Args...)`. ArgNames is parallel to Args; an invalid Symbol
    // marks a positional argument, a valid one a `name: value` argument.
    // BlockArg is invalid when the call carries no trailing `do ... end` block.
    struct Call
    {

        Core::SourceRange Loc;
        ExprId Callee;
        ExprList Args;
        SymbolList ArgNames;
        ExprId BlockArg;
    };

    // `Object[Args...]`
    struct Index
    {

        Core::SourceRange Loc;
        ExprId Object;
        ExprList Args;
    };

    // `Object.Name` — the receiver side of a call or a bare field access.
    struct Member
    {

        Core::SourceRange Loc;
        ExprId Object;
        Symbol Name;
    };

    // `Vector<T>` / `alloc<K, V>` in value position — a name explicitly
    // instantiated with type arguments. Base is whatever the instantiation
    // applies to (an Identifier, or a Member for `Alloc.alloc<T>`); the call
    // and member accesses that follow wrap this node, as in
    // `Call(Member(GenericInst(Vector, [T]), new))`.
    struct GenericInst
    {

        Core::SourceRange Loc;
        ExprId Base;
        TypeList Args;
    };

    struct SizeOf
    {

        Core::SourceRange Loc;
        TypeId Type;
    };

    // `*Operand` in value position.
    struct Deref
    {

        Core::SourceRange Loc;
        ExprId Operand;
    };

    // String interpolation: an alternating run of StringLiteral chunks and
    // embedded expressions, in source order.
    struct Interp
    {

        Core::SourceRange Loc;
        ExprList Parts;
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

    [[nodiscard]] constexpr ExprKind KindOf ( const ExprNode &Node )
    {
        return static_cast<ExprKind>( Node.index() );
    }

} // namespace Frontend

} // namespace Volt
