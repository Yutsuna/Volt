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

    struct SuperExpr
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

    // The address of a resolved Method, as a value — never written by the
    // parser; only a lowering pass (ClosureLifting) synthesizes this, to name
    // a function it has just lifted without going through ordinary member/
    // free-function name lookup. Inert like SizeOf/GenericInst: nothing to
    // descend into, the Decl is already resolved by construction.
    struct FuncAddr
    {

        Core::SourceRange Loc;
        DeclId Target;
    };

    // `( Value : Type )` — an explicit type ascription, not a cast: it
    // constrains an otherwise-unconstrained value (chiefly an int/float
    // literal) to `Type` instead of the default the literal would infer.
    // Core rather than sugar for the same reason as `ArrayLit`/`HashLit`
    // (`core-ast.md`): resolving `Type` and constraining `Value` against it
    // needs `TypeChecker`, so lowering it away earlier would need types.
    struct TypedExpr
    {

        Core::SourceRange Loc;
        ExprId Value;
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

    // `.method(args...)` shorthand used in `when .even?` or predicate matching.
    struct DotCall
    {

        Core::SourceRange Loc;
        Symbol Method;
        ExprList Args;
        SymbolList ArgNames;
    };

    // `Lhs |> Rhs`
    struct Pipeline
    {

        Core::SourceRange Loc;
        ExprId Lhs;
        ExprId Rhs;
    };

    // `case [Target] when Pattern1, Pattern2 [then] Body... [else ElseBody...] end`.
    // Target is invalid for a `case` without target expression (defaults to true matching).
    // Clauses is a StmtList of StmtIds referring to WhenClause nodes.
    // CaseLowering (order 22) folds Target into each clause's patterns and moves it here —
    // Scrutinee keeps the scrutinee sub-expression reachable for ScopeResolver/TypeChecker
    // without a backend having to re-derive it from folded patterns (rules/core-ast.md).
    struct CaseExpr
    {

        Core::SourceRange Loc;
        ExprId Target;
        ExprId Scrutinee;
        StmtList Clauses;
        StmtList ElseBody;
    };

    enum class ESectionKind : std::uint8_t
    {

        InstanceMethod,
        Operator,
        StaticCapture
    };

    struct Lambda
    {
        using Self = Lambda;
        Core::SourceRange Loc;
        ParamList Params;
        ExprId ReturnType;
        ExprId Body;
    };

    struct Section
    {
        using Self = Section;
        Core::SourceRange Loc;
        ESectionKind Kind;
        Symbol Target;
        ExprId TargetExpr;
        ExprList Args;
        bool bNegated;
        // Set only for ESectionKind::Operator — the raw token behind
        // `Target`'s spelling. A lowering needs this to build a `Binary`/
        // `Unary` node (the two the operator's spelling could be), never a
        // `Member`+`Call`: that shape is exempt from any resolution on a
        // primitive receiver (rules/core-ast.md's operator contract), so it
        // is unreachable outside a Binary/Unary node's own dispatch.
        TokenKind Op = TokenKind::Eof;
    };

    struct Composition
    {
        using Self = Composition;
        Core::SourceRange Loc;
        ExprId Lhs;
        ExprId Rhs;
    };

    struct RaiseExpr
    {
        using Self = RaiseExpr;
        Core::SourceRange Loc;
        ExprId Exception;
    };

    struct BeginExpr
    {
        using Self = BeginExpr;
        Core::SourceRange Loc;
        StmtList Body;
        StmtList RescueClauses;
        StmtList EnsureBody;
    };

    // `if Cond [then] ... [else ...] end`, and `unless` after the parser has
    // negated its condition. An `elsif` chain is a nested If living in the
    // Else branch — no separate node needed.
    //
    // An expression, like CaseExpr and BeginExpr, and for the same reason:
    // `val = if c ... end` is ordinary Volt, so the node has to be able to
    // carry a value. Statement position is reached through ParseStatement's
    // default arm, which wraps it in an ExprStmt.
    struct If
    {
        using Self = If;
        Core::SourceRange Loc;
        ExprId Cond;
        StmtList Then;
        StmtList Else;
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
