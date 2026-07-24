# MiddleEnd Specification: Type Inference & Checking (`TypeChecker`)

The `TypeChecker` pass (`source/Volt/Sema/Private/Passes/TypeChecker.cpp`, Pass Order 30) performs bidirectional type inference, constraint solving, operator resolution, and assignability checking across the AST.

---

## Architectural Layout

To maintain clean separation of concerns, `TypeChecker` is modularized under `source/Volt/Sema/Private/Passes/TypeChecker/`:

| Module Component | File Path | Responsible For |
|---|---|---|
| `ExprInferencer` | `ExprInferencer.cpp` | Bottom-up & top-down type inference over expressions |
| `DeclStmtWalker` | `DeclStmtWalker.cpp` | Declaration signatures, control flow, block return types |
| `TypeCompat` | `TypeCompat.cpp` | Unified assignability predicate `IsAssignable` |
| `MemberResolver` | `MemberResolver.cpp` | Method dispatch, property access, operator resolution |
| `LiteralInferencer` | `LiteralInferencer.cpp` | Unconstrained literal tracking and refinement |
| `TypeCheckerConstraint` | `TypeCheckerConstraint.cpp` | Recursive upstream constraint propagation (`ConstrainExprType`) |
| `TypeCheckerContext` | `TypeCheckerContext.hpp` | Per-file inference state, constraint queues, `UnitTypes` maps |

---

## Bidirectional Inference Mechanics

Type checking operates using a combination of bottom-up synthesized types and top-down expected types.

### Imperative Ordering Rule
At every assignment, declaration, or function argument site, `TypeChecker` MUST observe the strict ordering:

$$\text{Infer Subexpressions} \longrightarrow \text{Apply Constraints} \longrightarrow \text{Verify Assignability}$$

> **Critical Implementation Requirement**: Applying constraints before inferring child expressions freezes child nodes prematurely, causing parent expressions to memoize incorrect unconstrained types and preventing AST traversal.

### Unconstrained Refinement Walkthrough
1. **Initial Unconstrained Synthesis**: Encountering literal `10` synthesizes an unconstrained `IntLiteral` type with default fallback `Int32`.
2. **Top-Down Constraint Application**: Passing `10` into `fn process(val: UInt64)` invokes `ConstrainExprType(ExprId, TargetType)`.
3. **Upstream Refinement**: `ConstrainExprType` recurses into subexpressions, refining `10` directly to `UInt64` in `UnitTypes`.

---

## Assignability Contract (`TypeCompat::IsAssignable`)

All type compatibility checks across the 5 assignment sites use the unified predicate `TypeCompat::IsAssignable(SourceType, TargetType, EAssignSite)`:

### The 5 Assignment Sites (`EAssignSite`)
1. **`LocalDecl`**: Variable initialization (`let x: Target = expr`).
2. **`Assign`**: Variable reassignment (`x = expr`).
3. **`Return`**: Function return statements (`return expr`).
4. **`ImplicitReturn`**: Final block expression value.
5. **`ParamDefault`**: Default parameter value expressions.

### Assignability Rules
- **Exact Match**: `SourceType == TargetType` -> Always Assignable.
- **Nil Assignment**: `Nil` (`@[Literal(NilLiteral)]`) is assignable to any `Pointer` type.
- **Scalar Widening**: Same-family scalar widening without precision loss (e.g., `Int32` to `Int64`, `UInt32` to `UInt64`) is allowed. Narrowing (e.g., `Int64` to `Int32`) is **strictly forbidden**.
- **Nilable Types (`T?`)**: Unsupported and **loudly refused** at compile time (`nilable types are not implemented`).

---

## Method and Operator Resolution (`MemberResolver`)

Method calls (`o.method()`), binary operators (`a + b`), unary operators (`!x`), and index expressions are resolved uniformly by `MemberResolver`:

```
                       AST Node (Binary / Unary / Member)
                                       │
                                       ▼
                       Lookup Receiver Layout in TypeStore
                                       │
            ┌──────────────────────────┴──────────────────────────┐
            ▼                                                     ▼
 [Receiver is Primitive / Pointer]                     [Receiver is Nominal Struct / Class]
            │                                                     │
            ▼                                                     ▼
 Backend Emits Machine Instruction               Lookup Method Signature in `TypeStore`
 (e.g., `i32.add` in LLVM/VM/WASM)                                │
                                                                  ▼
                                                 Record Resolution in `UnitCallees`
                                                 (Backend emits call to `CalleeEntry`)
```

### Protocol Uniformity
- **No AST Mutation**: Operator calls do not generate extra AST call nodes. The original `Binary` / `Unary` AST node remains unchanged.
- **`UnitCallees` Snapshot**: Resolutions are written once into `UnitCallees` (`CalleeMap.hpp`) at the end of `TypeChecker` (Order 30). Backends inspect `UnitCallees` to distinguish native machine operations from stdlib method invocations.
- **Unresolved Operator Bug Policy**: If neither a primitive layout operation nor a method resolution exists, `TypeChecker` emits a semantic failure diagnostic (`UnknownMember` / `OperatorNotFound`).
