# Frontend Specification: Frontend Contracts & Verification (`AstInvariant`)

This document defines the strict output contracts of the Volt Frontend and how they are mechanically validated by the compiler test harness.

---

## The Frontend Hand-off Contract

When the frontend syntax phase finishes (`volt parse --lowered`), it produces an `AstContext` containing a fully desugared AST. The middle-end (`Sema`) expects this AST to satisfy three fundamental invariants:

### Invariant 1: Absolute Elimination of Sugar Nodes
Zero instances of the 9 syntactic sugar AST nodes may survive the lowering pipeline.

| Sugar Node Variant | Enum Marker | Eliminating Pass |
|---|---|---|
| `Expr::Interp` | `VOLT_EXPR_SUGAR` | `InterpLowering` (Pass 26) |
| `Expr::Index` | `VOLT_EXPR_SUGAR` | `IndexLowering` (Pass 25) |
| `Expr::DotCall` | `VOLT_EXPR_SUGAR` | `DotCallLowering` (Pass 23) |
| `Expr::Section` | `VOLT_EXPR_SUGAR` | `FunctionalLowering` (Pass 8) |
| `Expr::Composition` | `VOLT_EXPR_SUGAR` | `FunctionalLowering` (Pass 8) |
| `Expr::Pipeline` | `VOLT_EXPR_SUGAR` | `PipelineLowering` (Pass 9) |
| `Expr::JsxElement` | `VOLT_EXPR_SUGAR` | `JsxLowering` (Pass 20) |
| `Expr::JsxFragment` | `VOLT_EXPR_SUGAR` | `JsxLowering` (Pass 20) |
| `Expr::JsxText` | `VOLT_EXPR_SUGAR` | `JsxLowering` (Pass 20) |

### Invariant 2: Type Independence (Un-typed AST)
The frontend output AST contains **zero type binding information**:
- Expression nodes do NOT store type IDs or inferred types.
- Literals (e.g. `10`, `"hello"`) remain raw, unconstrained AST values.
- Identifiers remain plain string symbols; no scope binding or declaration pointers exist on the AST nodes.

### Invariant 3: Zero Type Hardcoding (`ZeroHardcode`)
The frontend never hardcodes built-in compiler type names (`Int`, `String`, `Array`, `Bool`). Method names produced during lowering (`"to_string"`, `"[]"`, `"[]="`) are plain string symbols passed to method lookup in `Sema`. Type signatures reside exclusively in the Volt standard library (`source/Lib/`).

---

## Automatic Mechanical Verification (`AstInvariant`)

The `AstInvariant` pass (`source/Volt/Sema/Private/Passes/AstInvariant.cpp`, Pass Order 40) is automatically executed:
1. At the end of every frontend pipeline invocation (`volt parse --lowered`).
2. Before `ScopeResolver` (Order 30) and `TypeChecker` (Order 35) in full compilation runs (`volt build`, `volt check`).
3. Across the entire test suite via `tests/AstInvariant.cmake`.

### `AstInvariant` Validation Logic
`AstInvariant` sweeps all `AstContext` arenas by index. If it detects any AST node variant tagged with `VOLT_EXPR_SUGAR` or invalid tree structural links, it fails loudly with an error trace:

```cpp
void InvariantChecker::CheckExpr(ExprId Id)
{
    const ExprNode &Node = Context.Expr(Id);
    std::visit(Meta::Overloaded{
        [&](const Expr::Interp &)     { Fail(Id, "Interp sugar node survived lowering"); },
        [&](const Expr::Index &)      { Fail(Id, "Index sugar node survived lowering"); },
        [&](const Expr::DotCall &)    { Fail(Id, "DotCall sugar node survived lowering"); },
        [&](const Expr::Section &)    { Fail(Id, "Section sugar node survived lowering"); },
        [&](const Expr::Composition &){ Fail(Id, "Composition sugar node survived lowering"); },
        [&](const Expr::Pipeline &)   { Fail(Id, "Pipeline sugar node survived lowering"); },
        [&](const Expr::JsxElement &) { Fail(Id, "JsxElement sugar node survived lowering"); },
        [&](const Expr::JsxFragment &){ Fail(Id, "JsxFragment sugar node survived lowering"); },
        [&](const Expr::JsxText &)    { Fail(Id, "JsxText sugar node survived lowering"); },
        [&](const auto &)             { /* Valid desugared core node */ }
    }, Node);
}
```

---

## Verification Commands

To verify frontend correctness, formatting, and build compliance during development:

```bash
# 1. Rebuild compiler and run full test suite (includes AstInvariant checks)
volt-build format test

# 2. Inspect desugared AST output for a test file
volt parse --lowered tests/fixtures/syntax_test.vl

# 3. Run static code analysis
volt-build tidy
```
