# Frontend Architecture — Parsing & Syntactic Lowering

## Role of the Frontend
The role of the Volt Frontend is to transform raw source code into a clean **Value Abstract Syntax Tree (AST)** and **desugar all syntactic sugar**.

The Frontend is **lazy**: it never attempts to guess or lock down the type of expressions or identifiers when there is ambiguity. Its sole objective is to produce an AST free of syntactic ambiguity, ready to be processed by the MiddleEnd.

---

## Frontend Components

### 1. Lexer (`source/Volt/Frontend/Lexer/`)
- Transforms source text into a stream of tokens (`Token`).
- Defined declaratively via token manifests (`TokenKind.inl`).

### 2. Parser (`source/Volt/Frontend/Parser/`)
- A combination of a **Pratt** expression parser (based on precedence / binding power) and a **recursive descent** parser for declarations and statements.
- Constructs the AST in arena memory (`Arena<Node, Id>`) without raw pointers (Value AST).

### 3. Syntactic Lowering (`EPassKind::Lowering`)
These passes rewrite syntactic sugar into canonical AST forms **before** any type analysis. They correspond exactly to what `volt parse --lowered` executes, in the order defined by the manifest `Sema/PassList.inl` (the numbering represents the pass `Order`, not a contiguous counter). Each pass **sweeps the arena by index** and never holds an arena reference across an `Add()` invocation — the mandatory idiom is documented in [`rules/ast-rewrite.md`](rules/ast-rewrite.md).

- **8 — `FunctionalLowering`**: Operator/method sections (`&.+ 5`, `&.trim`), captures (`&Math.square`), compositions (`>>`) → canonical `Lambda` nodes with unique symbols generated via `AstContext::MakeUniqueSymbol`.
- **9 — `PipelineLowering`**: `x |> f` → `f( x )`.
- **12 — `EnumLowering`**: Enums → structs + static constants.
- **15 — `MacroExpansion`**: Declarative macros + `@[...]` annotations. Removes `MacroDef` nodes from `TopDecls` at the end of the pass (the node slot remains in the arena).
- **16 — `MagicExpansion`**: `__FILE__` / `__LINE__` and related magic constants.
- **20 — `JsxLowering`**: `<Button />` → `Volt.JSX.create_element( ... )`.
- **22 — `CaseLowering`**: `case/when` → flat list of `WhenClause` entries (`pattern === target`), never an `If` chain (§4.3 of [`rules/core-ast.md`](rules/core-ast.md): `CaseExpr` remains **core**).
- **23 — `DotCallLowering`**: A `.method` in statement position → `self.method( ... )`. Runs immediately after `CaseLowering` to avoid stealing `DotCall` nodes from `when .even?` pattern arms.
- **24 — `AssignLowering`**: `x op= v` → `x = x op v`. The base operator is **derived from token spelling** (`+=` minus its trailing `=`), rather than a hardcoded lookup table.
- **25 — `IndexLowering`**: `o[ a ]` → `o.[]( a )`, `o[ a ] = v` → `o.[]=( a, v )`. The compound case `o[ a ] += v` falls out automatically from composition with `AssignLowering`: no dedicated code required.
- **26 — `InterpLowering`**: `"a#{ x }b"` → left-associative string concatenation via `x.to_string`. Runs after `MacroExpansion`, as macros may generate `#{ ... }` interpolation constructs.

`"to_string"`, `"[]"`, `"[]="` are **method names**, not Volt type names: the `ZeroHardcode` rule remains strictly preserved. See [`rules/zero-hardcode.md`](rules/zero-hardcode.md).

---

## Frontend Output Artifact
At the exit of the Frontend (`volt parse --lowered`), the AST is:
- **Fully Desugared**: None of the **10 sugar nodes** (`Interp`, `Index`, `DotCall`, `Section`, `Composition`, `Pipeline`, `JsxElement/Fragment/Text`, `ArrayLit`, marked `VOLT_EXPR_SUGAR` in `Nodes.inl`) survive `TypeChecker` — `ArrayLit` is the one exception lowered by `TypeChecker` itself (a post-walk sweep) rather than an `EPassKind::Lowering` pass; see [`rules/core-ast.md`](rules/core-ast.md). The `AstInvariant` pass (Order 40) validates the census **mechanically** on every build. See [`rules/meta-first.md`](rules/meta-first.md).
- **Strictly Un-typed**: Literals such as `10` remain raw and unconstrained.
- **MiddleEnd Ready**: No early type assumptions are locked in prematurely.
