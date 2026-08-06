# MiddleEnd Architecture — Semantic Analysis & Lazy-Strict Typing

## Role of the MiddleEnd
The Volt MiddleEnd takes the desugared AST produced by the Frontend and performs **Scope Resolution**, **Type Binding**, and **Semantic Constraint Propagation**.

Its foundational philosophy is **"Lazy Dynamic, but Strict Typing"**.

---

## Philosophy: "Lazy Dynamic, but Strict Typing"

The MiddleEnd never rushes to freeze a type prematurely. If a literal or variable lacks an explicit type annotation, it is maintained in an **"unconstrained / potentially-this-type"** state and refined as it is used down the pipeline. However, as soon as a type is **explicitly declared**, it becomes **strict and immutable** (no implicit type coercions allowed).

### Concrete Walkthrough Example:

```volt
def func( a : UInt64 ) -> Int32
   a
end

b = 10
func b
```

### Step-by-Step Processing in the MiddleEnd:

1. **Implicit Assignment (`b = 10`)**:
   - The Frontend produces an integer literal `10` and the declaration of `b`.
   - The MiddleEnd registers `10` as an **unconstrained** `IntLiteral` with `Int32` as a fallback default.
   - `b` receives the unconstrained expression type of `10`. The type of `b` is **not locked** to `Int32`.

2. **Function Call (`func b`)**:
   - The signature of `func` requires a parameter `a` of type **`UInt64`**.
   - During call argument checking (`CheckCallArgs`), the constraint `UInt64` is **propagated upstream** (recursive constraint propagation via `ConstrainExprType`) onto argument `b`, and subsequently onto its literal initializer `10`.
   - Both the literal `10` and the variable `b` are thus refined and locked to **`UInt64`**.

3. **Explicit Return Value (`a` of type `UInt64` to `Int32`)**:
   - Parameter `a` is **explicitly typed** as `UInt64`. Its type is known with certainty and is **strict**.
   - The function body attempts to return `a` while the function signature specifies an `Int32` return type.
   - **Immediate Semantic Error**: Because `a` is explicitly `UInt64`, Volt strictly rejects all implicit conversions to `Int32`.

---

## MiddleEnd Passes (`EPassKind::Analysis`)

Type binding (`Sema::BindUnitTypes`, `Layout/TypeBinder.cpp`) executes **before** the parallel pass phase, inside the Driver's serial execution seam: a literal `10` in a user file resolves to `Int32` declared in `source/Lib/`. This binding is cross-unit and cannot be run as a per-file pass. It also registers **module names** (`TypeStore::DeclareModule`): a `module` is a namespace, not a nominal type — its methods are free functions.

### 1. Order 10 — `ScopeResolver`
- Constructs the `ScopeTable` (`Method`/`Block`/`Branch` scopes), declares parameters and local variables, and computes closure captures along with their `bEscapes` status.
- Rejects redeclarations within the same scope while permitting child scope shadowing.

### 2. Order 30 — `TypeChecker` (`Sema/Private/Passes/TypeChecker/`)
- Bidirectional inference over `UnitTypes`; `UnconstrainedLiterals` / `UnconstrainedVarInitializers` manage the lazy refinement of unconstrained literals.
- **Imperative ordering at every assignment site: infer → constrain → verify.** Constraining before inferring freezes child expressions (the parent is memoized and AST traversal never occurs).
- **Comprehensive Assignability**: A unified predicate `TypeCompat::IsAssignable` (+ `CheckAssignable( ..., EAssignSite )`) is wired across all 5 assignment sites (`LocalDecl`, `Assign`, `Return`, final block body value, parameter default). An **explicitly declared** type is strict: zero implicit conversions, **except** same-family scalar widening without narrowing — a language semantic design decision forced by `hash -> UInt64`, detailed in [`rules/zero-hardcode.md`](rules/zero-hardcode.md).
- **Operators**: `MemberType` records resolution in `CalleeResolution` for `Binary`/`Unary` as well as `Member` — for primitive/pointer memory layouts, the backend emits a machine instruction; otherwise it emits a method call. Zero extra passes, zero extra AST nodes. An operator exempt from a body must **still** be declared.
- `Nil` (`@[Literal( NilLiteral )]`) is assignable to any `Pointer`; `T?` (`NilableType`) is **loudly refused** (`nilable types are not implemented`).
- Arity validation, instance vs static member contexts (`bStaticContext`), free function resolutions.

### 3. Order 40 — `AstInvariant`
The safety check that makes "Zero Debt" **structural** rather than transient. Creates zero AST nodes (the only way to run after `TypeChecker` without breaking structural invariants). Performs two checks, both producing hard errors:
- **Zero Residual Sugar**: Verifies no `VOLT_EXPR_SUGAR` node variants exist in the arena.
- **Total Value Typing**: Every expression in a value position must be typed, excluding `Context.Metadata` and generic template bodies (per the backend input contract in [`rules/core-ast.md`](rules/core-ast.md)).

Counters are reported by `volt check --metrics` (`PassStats`, aggregated via reflection).

> **Structural Invariant**: No node-creating pass may run after `TypeChecker`. This guarantees that every AST node seen by a backend possesses a resolved type.

---

## MiddleEnd Output Artifact
At the exit of the MiddleEnd, the AST/HIR delivered to the backend (see [`rules/core-ast.md`](rules/core-ast.md) for the complete contract — **25 core nodes**, with `CalleeResolution` and `ClosureEnvFrame` fully precomputed) is:
- **100% Resolved**: Every identifier is bound to its declaration site.
- **100% Typed**: With a single nuance defined by the contract: typed immediately in concrete code, **typed after substitution** inside generic bodies (`T` in `Array<T>` becomes a type only upon monomorphization during codegen).
- **100% Validated**: All semantic diagnostics have been emitted.
