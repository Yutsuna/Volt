# Frontend Specification: Lexical Analysis (Lexer)

The Lexer module (`source/Volt/Frontend/Lexer/`) converts UTF-8 source buffer bytes into a flat sequence of immutable `Frontend::Token` structures.

---

## Architectural Principles

1. **Declarative Token Definition**: Every token type is specified in `TokenKind.inl`. Tokens are categorized into keywords, punctuation, literals, identifiers, and control tokens.
2. **Zero Allocation Tokenization**: Tokens store 32-bit `SourceLocation` spans pointing directly into `SourceManager` memory buffers. No string copies or memory allocations take place during lexing.
3. **Keyword Interning**: Keywords are resolved through string lookup tables generated at compile time from `TokenKind.inl`.

---

## Data Structures

### `ETokenKind` (`TokenKind.inl`)
Defined via macro expansion in `source/Volt/Frontend/Public/Volt/Frontend/Lexer/TokenKind.inl`.

Key categories defined in the manifest:
- **Literals**: `IntLiteral`, `FloatLiteral`, `CharLiteral`, `StringLiteral`, `SymbolLiteral`.
- **Keywords**: `fn`, `let`, `var`, `class`, `struct`, `enum`, `if`, `else`, `case`, `when`, `while`, `return`, `break`, `next`, `true`, `false`, `nil`, `self`, `super`.
- **Punctuation & Operators**: `Plus`, `Minus`, `Star`, `Slash`, `Percent`, `Amp`, `Pipe`, `Caret`, `Tilde`, `Equal`, `EqualEqual`, `BangEqual`, `PipeGreater` (`|>`), `AmpDot` (`&.`), `DotDot` (`..`), `DotDotDot` (`...`), `FatArrow` (`=>`), `ThinArrow` (`->`).
- **Control Tokens**: `Eof`, `Error`, `Newline`, `Indent`, `Dedent`.

### `Token` (`source/Volt/Frontend/Public/Volt/Frontend/Lexer/Token.hpp`)

```cpp
struct Token
{
    ETokenKind Kind = ETokenKind::Eof;
    Core::SourceLocation Location;
    Core::Symbol TextSymbol; // StringInterner symbol handle for identifier/literal text
};
```

---

## Lexing Mechanics (`Frontend::Lexer`)

The primary entry point is `Frontend::Lexer::LexNext()` in `source/Volt/Frontend/Private/Lexer/Lexer.cpp`.

### 1. Identifier vs Keyword Differentiation
When lexing alphanumeric sequences starting with `[a-zA-Z_]`:
1. The lexer reads characters until encountering a non-identifier character (`[a-zA-Z0-9_!?]`).
2. Identifiers in Volt may end with trailing `?` or `!` (e.g., `nil?`, `save!`).
3. The resulting string is interned into `StringInterner`.
4. If the string matches a keyword from `TokenKind.inl`, `Kind` is set to the corresponding keyword `ETokenKind`; otherwise, it is categorized as `ETokenKind::Identifier`.

### 2. Number Parsing Logic
Numbers are scanned in `LexNumber()`:
- Integer prefixes: `0x` (hexadecimal), `0b` (binary), `0o` (octal).
- Fractional components: A decimal point `.` followed by digits (excluding `..` range operators) transitions the token kind to `ETokenKind::FloatLiteral`.
- Numerical separators: Underscores in numbers (e.g., `1_000_000`) are stripped before parsing.

### 3. String & Interpolation Scanning
Strings can be single-quoted (`'...'`), double-quoted (`"..."`), or multiline (`"""..."""`):
- Single-quoted strings treat backslashes literally and do not support interpolation.
- Double-quoted strings scan for interpolation markers `#{`. When `#{` is encountered inside a double-quoted string:
  1. The lexer emits an `ETokenKind::InterpStringBegin` or `ETokenKind::InterpStringMid`.
  2. It pauses string lexing and pushes an interpolation mode state onto its internal lexer stack.
  3. The parser processes normal expression tokens within the interpolated block until matching `}`.
  4. The lexer resumes string scanning, emitting `ETokenKind::InterpStringEnd`.

### 4. JSX Parsing Mode Switch
When the lexer encounters `<` in an expression context where a JSX element can begin:
- The parser notifies the lexer to enter `EJsxMode::Tag` or `EJsxMode::ChildText`.
- In `EJsxMode::ChildText`, whitespace and raw text are lexed as `ETokenKind::JsxText` until `{` or `<` is encountered.

---

## Error Handling

Lexical errors (e.g., unclosed string literals, invalid numeric characters) produce an `ETokenKind::Error` token containing the diagnostic location. The lexer never crashes or throws exceptions; it emits diagnostic tokens allowing the parser to attempt error recovery.
