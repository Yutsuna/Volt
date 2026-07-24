# Frontend Specification: Parser Architecture & Grammar

The Parser module (`source/Volt/Frontend/Parser/`) constructs the un-typed Value AST from the token stream emitted by the Lexer.

It employs a hybrid parsing strategy:
1. **Pratt Parser (Top-Down Operator Precedence)**: Handles expressions, binary/unary operations, member accesses, call chains, sections, and pipelines.
2. **Recursive Descent Parser**: Handles top-level declarations, statements, blocks, control-flow constructs, type annotations, and JSX syntax.

---

## The Pratt Expression Engine

Expression parsing is driven by Pratt precedence tables declared in `Pratt.inl` (`source/Volt/Frontend/Public/Volt/Frontend/Parser/Pratt.inl`).

### Precedence Table (`EBindingPower`)

| Rank | Operator / Syntax | Binding Power (Left / Right) | Parselet Function |
|---|---|---|---|
| 1 | Assignment (`=`, `+=`, `-=`, etc.) | 10 / 9 (Right-associative) | `ParseAssign` |
| 2 | Ternary (`? :`) | 15 / 14 (Right-associative) | `ParseTernary` |
| 3 | Pipeline (`\|>`) | 20 / 21 (Left-associative) | `ParseInfix` |
| 4 | Logical Or (`\|\|`, `or`) | 30 / 31 (Left-associative) | `ParseInfix` |
| 5 | Logical And (`&&`, `and`) | 40 / 41 (Left-associative) | `ParseInfix` |
| 6 | Equality (`==`, `!=`, `===`) | 50 / 51 (Left-associative) | `ParseInfix` |
| 7 | Relational (`<`, `<=`, `>`, `>=`) | 60 / 61 (Left-associative) | `ParseInfix` |
| 8 | Range (`..`, `...`) | 70 / 71 (Left-associative) | `ParseInfix` |
| 9 | Additive (`+`, `-`) | 80 / 81 (Left-associative) | `ParseInfix` |
| 10 | Multiplicative (`*`, `/`, `%`) | 90 / 91 (Left-associative) | `ParseInfix` |
| 11 | Unary (`-`, `!`, `~`, `&`) | 100 (Prefix) | `ParsePrefix` |
| 12 | Member Access (`.`), Call (`()`), Index (`[]`) | 120 / 121 (Left-associative) | `ParsePostfix` |

### Pratt Algorithm Mechanics
`Frontend::Parser::ParseExpr(int MinBp)` operates as follows:

```cpp
ExprId Parser::ParseExpr(int MinBp)
{
    Token Tok = Advance();
    PrefixParselet Prefix = GetPrefixParselet(Tok.Kind);
    if (!Prefix) { EmitError("Expected expression"); return InvalidExprId; }

    ExprId Left = (this->*Prefix)(Tok);

    while (MinBp < GetLeftBindingPower(PeekKind()))
    {
        Tok = Advance();
        InfixParselet Infix = GetInfixParselet(Tok.Kind);
        Left = (this->*Infix)(Left, Tok);
    }
    return Left;
}
```

---

## Recursive Descent Parsing

### Declarations (`AtDeclaration`, `ParseDeclaration`)
Declarations occur at top-level or inside class/struct/enum bodies:
- **Function Declarations (`fn`)**: Parses generic parameters (`<T>`), parameters `(a: Int, b: String)`, optional return type `-> ReturnType`, and block body `{ ... }` or expression body (`=> expr`).
- **Variable Declarations (`let` / `var`)**: Parses binding patterns, optional explicit type annotations (`: Type`), and initialization expressions (`= expr`).
- **Class & Struct Declarations (`class` / `struct`)**: Parses fields, methods, constructor definitions, and inheritance specifiers (`: BaseClass`).
- **Enum Declarations (`enum`)**: Parses simple enum items or algebraic payload variants (`case Variant(Type)`).
- **Macro Declarations (`macro`)**: Parses declarative AST macro matchers.

### Control Flow Statements (`ParseStmt`)
- **`if` / `else`**: Parses condition expression, then block, and optional `else` / `else if` branch.
- **`case` / `when`**: Parses target expression and a sequence of pattern arms (`when pattern => body`).
- **`while` / `for`**: Loop constructs with optional break/next labels.
- **`return` / `break` / `next`**: Jump statements returning optional values.

---

## Special Parser Rules

### 1. Operator and Method Sections (`&.`, `&.+`)
Volt supports concise method and operator captures:
- `&.+ 5`: Parsed into `Frontend::Section` node with left-hand placeholder and operator `+`, right-hand `5`.
- `&.trim`: Parsed into `Frontend::Section` node capturing property/method `.trim`.
- `&Math.square`: Parsed into `Frontend::Section` node referencing static method target.

### 2. Dot-Call Statements (`ParseDotCall`)
When a line begins with a leading dot in statement position (e.g. `.save()`), the parser constructs a `Frontend::DotCall` node. This represents an implicit call on `self` or a context receiver.

### 3. JSX Parsing (`ParseJsxElement`)
Upon encountering `<Identifier` in an expression position:
1. The parser enters JSX parsing mode.
2. Attributes are parsed as `JsxAttribute` key-value pairs or spread attributes (`{...props}`).
3. Child elements are parsed recursively until matching `</Tag>`.
4. Self-closing tags (`<Tag />`) emit a `JsxElement` with an empty child list.

---

## Error Recovery Strategies

The parser is designed to be resilient during IDE usage and batch compilation errors:

- **Statement Resynchronization (`RecoverToStatement`)**: Upon encountering an unrecoverable syntax error within an expression or statement, the parser logs a diagnostic and skips tokens until reaching a synchronization boundary (semicolon, newline, `}`, `fn`, `let`, `class`).
- **Placeholder Nodes**: Failed sub-expression parses return an `InvalidExprId` or poison node, preventing cascade crash errors during subsequent compiler passes.
