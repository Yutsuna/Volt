# MiddleEnd Specification: Overview & Architecture

The MiddleEnd of the Volt compiler (`source/Volt/Sema/`) consumes the desugared Value AST produced by the Frontend and executes **Scope Resolution**, **Type Binding**, **Bidirectional Type Inference**, and **Semantic Constraint Enforcement**.

The MiddleEnd bridge bridges the gap between raw syntactic expressions and backend-ready, fully typed Core ASTs.

---

## Core Philosophy: "Lazy Dynamic, but Strict Typing"

Volt combines the ergonomic flexibility of dynamic languages with the safety and performance of statically typed systems.

### 1. Lazy Unconstrained Initialization
When an expression or local variable is introduced without an explicit type annotation (e.g., `x = 10`), the MiddleEnd does **not** instantly lock `x` to `Int32`. Instead, `10` is registered as an `IntLiteral` within an **unconstrained state** (`UnconstrainedLiterals`). 

As `x` flows through downstream expressions, calls, and function return sites, constraints are propagated upstream recursively (`ConstrainExprType`). The variable `x` is refined and locked to a concrete layout (such as `UInt64`) only when an explicit constraint demands it.

### 2. Strict Explicit Boundaries
As soon as a type signature or variable is **explicitly declared** (e.g., `let x: UInt64 = 10` or `fn process(a: UInt64)`), that type boundary becomes **strict and immutable**. Volt forbids all implicit conversions or narrowing coercions across explicit type boundaries.

```
       [Unconstrained Literal: 10]
                  │
     (Flows into `fn process(a: UInt64)`)
                  │
                  ▼
   [Upstream Constraint Propagation] ──> Refines `10` to `UInt64`
                  │
                  ▼
   [Explicit Parameter Boundary `a: UInt64`]  (Strict Immutable Boundary)
                  │
     (Attempting implicit return to `Int32`)
                  │
                  ▼
   [HARD SEMANTIC FAILURE]  <── No implicit narrowing or cross-type conversion
```

---

## Serial Type Binding vs Parallel Passes

Semantic execution is cleanly divided into two phases within the Driver:

```
                          ┌───────────────────────────┐
                          │     Driver Parallel       │
                          │   Frontend & Lowering     │
                          └─────────────┬─────────────┘
                                        │ (Desugared ASTs)
                                        ▼
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ Serial Cross-Unit Seam (`Sema::BindUnitTypes`)                           │
  │  - Maps `10` to standard library nominal `Int32` in `source/Lib/`         │
  │  - Registers module namespaces (`TypeStore::DeclareModule`)               │
  │  - Publishes public type structures across compilation units             │
  └─────────────────────────────────────────┬────────────────────────────────┘
                                            │
                                            ▼
                          ┌───────────────────────────┐
                          │     Driver Parallel       │
                          │   MiddleEnd Analysis      │
                          │ - ScopeResolver (Order 10)│
                          │ - TypeChecker   (Order 30)│
                          │ - UnusedChecker (Order 35)│
                          │ - AstInvariant  (Order 40)│
                          └───────────────────────────┘
```

### Serial Phase: `Sema::BindUnitTypes`
Because Volt types (like `Int32`, `String`, `Array`) are declared within the standard library (`source/Lib/`) rather than hardcoded in the C++ compiler (`rules/zero-hardcode.md`), type resolution is **cross-unit**. `Sema::BindUnitTypes` executes serially between units before parallel semantic analysis:
- Binds user-level type names to `NominalId` representations in `TypeStore`.
- Registers module declarations as namespaces (`TypeStore::DeclareModule`).
- Publishes public struct, class, and interface declarations.

### Parallel Phase: Analysis Passes
Per-file semantic passes (`EPassKind::Analysis`) run concurrently across compiled units:
- **Order 10 — `ScopeResolver`**: Constructs `ScopeTable` hierarchies and computes closure captures.
- **Order 30 — `TypeChecker`**: Performs bidirectional inference, constraint solving, and method resolution.
- **Order 35 — `UnusedChecker`**: Emits warnings for unused local variables and parameters.
- **Order 40 — `AstInvariant`**: Mechanically validates that the output Core AST satisfies backend expectations.

---

## Shared Semantic Data Structures

MiddleEnd state is stored out-of-band from AST nodes in reusable tables:

| Data Structure | Header Path | Primary Purpose |
|---|---|---|
| `ScopeTable` | `Sema/Scope/ScopeTable.hpp` | Symbol visibility scopes, block nestings, variable declarations, closure frames |
| `TypeStore` | `Sema/Layout/TypeStore.hpp` | Nominal type registry, memory layouts (`MemoryLayout`), field offsets |
| `UnitTypes` | `Sema/Layout/SemaType.hpp` | Per-unit map storing inferred `SemaTypeId` for every AST `ExprId` |
| `UnitCallees` | `Sema/Layout/CalleeMap.hpp` | Resolved callee resolutions (`CalleeEntry`) for method calls and operator overrides |
| `ClosureFrame` | `Sema/Layout/ClosureFrame.hpp` | Precomputed frame layout (offset, total size, alignment, heap escape flag `bEscapes`) |
