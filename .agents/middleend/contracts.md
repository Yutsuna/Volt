# MiddleEnd Specification: Semantic Verification (`AstInvariant`)

The `AstInvariant` pass (`source/Volt/Sema/Private/Passes/AstInvariant.cpp`, Pass Order 40) is the structural guard rail of the Volt MiddleEnd.

It ensures that the MiddleEnd hand-off contract (see [`rules/core-ast.md`](file:///home/Yutsuna/Projects/VoltLang/Volt/.agents/rules/core-ast.md)) is strictly enforced before code generation begins.

---

## Architectural Role: The Structural Invariant

In traditional compiler architectures, pass ordering bugs can cause untyped nodes or un-lowered sugar nodes to leak into backend emitters, causing cryptic backend crashes or invalid machine code generation.

Volt eliminates technical debt by turning correctness into a **mechanical structural invariant**:

> **The Structural Invariant**: No AST node-creating pass is permitted to execute after `TypeChecker` (Pass Order 30).

Because `AstInvariant` runs at Order 40 as a read-only pass, it guarantees that the AST state evaluated by `AstInvariant` is identical to the AST state seen by backends.

---

## The Two Hard Contracts Verified by `AstInvariant`

`AstInvariant` sweeps all `AstContext` arenas by index, validating two non-negotiable invariants:

### Contract 1: Complete Elimination of Sugar Nodes
Verifies that **zero syntactic sugar nodes** survive in the AST arena. The target sugar variants checked are:
1. `Expr::Interp` (String interpolation)
2. `Expr::Index` (Array/Hash bracket access)
3. `Expr::DotCall` (Implicit self dot calls)
4. `Expr::Section` (Operator/method sections)
5. `Expr::Composition` (Function composition `>>`)
6. `Expr::Pipeline` (Pipeline operator `|>`)
7. `Expr::JsxElement` (JSX element tag)
8. `Expr::JsxFragment` (JSX fragment `<>`)
9. `Expr::JsxText` (JSX child text)

If any sugar node variant is encountered, `AstInvariant` halts compilation with a hard fatal error (`SugarNodeSurvivedLowering`).

### Contract 2: Total Value Typing
Verifies that **every AST value expression has a resolved type** recorded in `UnitTypes`.

#### Exceptions Excluded from Typing:
- `Context.Metadata` nodes (compiler annotations).
- Non-value statements (`Stmt` variants like `Block`, `If`, `While`).
- Uninstantiated generic function bodies (e.g. `T` in `Array<T>`). Generic bodies are typed only after concrete substitution during monomorphization (`Monomorphizer` in BackendCore).

If an un-typed concrete value expression is encountered, `AstInvariant` halts compilation with a hard fatal error (`UntypedValueExpression`).

---

## Mechanical Test Execution

`AstInvariant` is automatically executed:
- At the end of every `volt check` or `volt build` invocation.
- Across every test file in the codebase via `tests/meson.build`.
- During CI runs of the full test suite.

Run the `AstInvariant` test target through the IDE (or filter `meson test` with `--suite golden`) to run the verification suite manually.
