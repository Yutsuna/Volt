# Rule: RAII & Ownership Model in Volt

This document defines the portable, versioned rules for Volt's RAII (Resource Acquisition Is Initialization) system, cleanup regions, and ownership model.

---

## 1. The Core Principle (Zero Special Cases)

> **The RAII machinery takes an object and does not care in the slightest what type it is. At its birth it calls the constructor (`initialize`); at its death it calls the destructor (`finalize`). That is all.**

- **Zero Hardcoding**: The C++ compiler NEVER hardcodes Volt type names (`String`, `Array`, `Proc`, `Exception`, etc.) or Volt method names (e.g. `"to_string"`) in RAII or lifetime analysis (`rules/zero-hardcode.md`).
- **Machine Backend**: All destructor calls, cleanup regions, and drop-on-reassign sequences are lowered into core AST nodes in the middle-end. Backends (LLVM, WASM, VM) remain strictly machine-only (`rules/backend-machine-only.md`).

---

## 2. Universal `finalize` (The C++ `= default` Rule)

- **Universal Destructors**: Every nominal type in Volt possesses a `finalize` destructor method. If none is explicitly defined in source, `SynthesizeFinalizeStubs` synthesizes one at the serial middle-end seam, exactly as C++ gives every class a defaulted destructor.
- **Trivial Finalization (`bTrivialFinalize`)**: A defaulted `finalize` with no fields to cascade, no ancestor destructors to call, and no hand-written body is marked `NominalType::bTrivialFinalize = true` (the direct counterpart of `std::is_trivially_destructible`).
- **No-Op Pruning**: `Raii::IsFinalizeCandidateNominal` checks `!bTrivialFinalize`. Trivial types emit zero cleanup boundaries and zero destructor calls.
- **Element Release in Containers**: Container element destruction (e.g. `Array<T>#finalize`) is implemented **in Volt standard library code**, iterating over elements when `not trivially_destructible? T`. The compiler NEVER synthesizes element-loop cascades in C++.

---

## 3. `trivially_destructible? T` Compile-Time Predicate

- **Compile-Time Trait**: `trivially_destructible? T` is a core compile-time type trait (`TypeTrait` node, keyword in `TokenKind.inl`), sibling to `sizeof`.
- **Compile-Time Branch Folding**: Inside generic instantiations (e.g. `Array<T>`), `ReinstantiateBody` evaluates the trait for concrete `T`. When `T` is trivially destructible, the `if not trivially_destructible? T` block is folded away at compile time.

---

## 4. Destructor Base Cascading (Inheritance)

- **Execution Order**: A synthesized destructor executes in precise order: own fields in reverse declaration order, followed by parent base classes.
- **AST-Based Parent Resolution**: Parent types are resolved off the AST during `SynthesizeFinalizeStubs`, because `NominalType::Super`/`Includes` are populated during the signature phase running after that seam.

---

## 5. Ownership Classification: Proven, Never Presumed

Volt classifies expressions into `EOwnership` states (`{ Owned, Borrowed, Moved }`).

| Expression Form | State | Rationale |
|---|---|---|
| `Identifier` / `InstanceVar` / field `Member` | `Borrowed` | Reads an existing place. |
| `Identifier` / `Member` resolving to a method with `bReturnsOwned` | `Owned` | Paren-less method invocation. |
| `Call` (or paren-less `Member`) with `bConstructs` | `Owned` | Object construction (`Type.new`). Certain. |
| `Call` with callee `bReturnsOwned` | `Owned` | Derived by monotone fixpoint over method bodies. |
| Literal (`@[Literal( Kind )]`) | `Owned` | Materializes a fresh value (cannot name an existing place). |
| All other forms | `Borrowed` | **The safe default**. |

- **Asymmetric Safety**: `Borrowed` is the default. A missed classification produces a counted leak (`RaiiOwnedWithoutCleanup`), NEVER a double-free or memory corruption.

---

## 6. Seam Placement: Core AST Only

- **Lowered Seam Order**: `Raii::InferReturnOwnership` and `Raii::InferParameterEscape` run at `Sema::LoweredSeamOrder()` (after all `EPassKind::Lowering` passes, before `TypeChecker`).
- **Core AST Operation**: The analysis operates strictly on desugared core AST (`Call` nodes), eliminating the need to guess how sugar (`Interp`, `Index`, `Pipeline`, `Section`) will lower.
- **Architectural Rule**: *"Move the question, never teach it a spelling."*

---

## 7. Parameter Escape vs. Return Ownership

- **`bReturnsOwned`**: Starts `false` and climbs. Must be proven owned before the caller releases the result.
- **`ParamEscapes`**: Starts `true` and falls. Must be disproven before the caller releases an argument temporary.
- **Double-Free Prevention**: If an argument might escape (e.g. `arr.push(s.dup)`), the caller does NOT release the temporary, delegating ownership to the callee.

---

## 8. Closure Literals & Indirect Calls

- **Closure Facts**: Facts for closure literals (`ClosureReturnsOwned`, `ClosureParamEscapes`) are recorded against the *literal expression ID* in `TypeCheckerContext` before closure lifting.
- **Opaque Function Pointers**: Calls through unknown function pointers default safely to argument escape (`ParamEscapes = true`).

---

## 9. Cleanup Regions & Unwind Boundaries

- **`CleanupRegion` Primitive**: Defines lifetime regions. `BeginExpr { Body, EnsureBody }` is merely its current middle-end AST representation.
- **Encapsulation Rule**: **Only `CleanupRegion.cpp` is permitted to construct `Frontend::BeginExpr`.**
- **Region Categories**: Scope regions (`StmtList` owning named locals) and Full-expression regions (statements owning materialized temporaries). Each region emits exactly ONE boundary.

---

## 10. Control Flow Exits & Unwinding

- **Fall-Through / Unhandled `raise` / Non-Local `break`**: Wrapped in `BeginExpr{ Body, EnsureBody }`.
- **`return` / Loop-Owned `break`/`next`**: Destructor calls are spliced directly before exit statements.
- **Expression Positions**: Exits nested inside `If`/`CaseExpr`/`BeginExpr` in expression position are fully discovered via `CollectNestedBlockExprs`.

---

## 11. Drop on Reassign

- **Reassignment Cleanup**: Writing to a local twice (`x = expr`) releases the previous value held by `x` via `RunDropOnReassign`, sequencing:
  ```
  __old = x;
  x = expr;
  __old.finalize();
  x;
  ```

---

## 12. Generic Bodies & Monomorphization

- Generic definition bodies defer concrete types; RAII sweeps run per instantiation in `Sema::ReinstantiateBody` over concrete monomorphized types.

---

## 13. Accounting Invariant & Verification

- **Identity**: `RaiiOwnedCreated == RaiiMoves + RaiiFinalizes + RaiiExplicitEscapes`
- **Metrics**: Counters on `PassStats` report RAII metrics via `volt check --metrics`.
