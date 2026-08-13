# Backend Architecture — Declarative Code Generation & Targets

The Backend consumes the middle-end's output and emits code. Its operating
principle is **declarative generation by pattern matching**:

- **Zero semantic analysis in the Backend.**
- **Zero type inference or resolution in the Backend.**
- **Zero uncertainty** — anything unclear was refused loudly upstream.

> **Input contract: [`rules/core-ast.md`](rules/core-ast.md).** Exactly
> **25 core nodes** (the 11 sugar nodes are gone, checked by `AstInvariant`),
> every value expression typed — outright in concrete code, after
> substitution inside a generic body. The contract is materialised for
> backends through four read-only tables per unit: `UnitTypes` (expression
> types), `UnitCallees` (`MiddleEnd/IR/CalleeMap.hpp` — the
> method-vs-machine-instruction protocol, snapshotted at the end of
> `TypeChecker`), `SynthesizedFunctions` (closure bodies synthesized by
> `ClosureLifting`), and the build-wide `TypeStore`'s `MemoryLayout`s.

## The 3 targets

```
                     +---> [1] volt run / repl        ---> BackendVM    (bytecode VM, hot reload)
Middle-end (core AST)-+---> [2] volt build --target wasm -> BackendWASM (self-contained .wasm encoder)
                     +---> [3] volt build              ---> BackendLLVM (LLVM IR, native AOT)
```

All three sit on the shared **BackendCore** layer and are ordinary Volt
modules under `source/Volt/Backend/`.

## The spec

| Document | Covers |
|---|---|
| [`backend/core-interfaces.md`](backend/core-interfaces.md) | `concept TargetBackend`, `BackendInput`/`UnitView`, node dispatch, the callee protocol, `Monomorphizer`, error policy, module wiring |
| [`backend/llvm.md`](backend/llvm.md) | 24-node → IR mapping, `LlvmBackend` internal architecture (service decomposition), instruction table, control flow, closures, EH tiers, optimisation & link pipeline |
| [`backend/vm.md`](backend/vm.md) | bytecode ISA (`Bytecode.inl` manifest), frames, dispatch, **hot reload** via the `FunctionTable` seam, REPL |
| [`backend/wasm.md`](backend/wasm.md) | module sections, layout → wasm types, JS bridge imports, the JSX dependency, `--target wasm` |
| [`backend/abi.md`](backend/abi.md) | `LayoutEngine` rules, object/string/array layout (stdlib-declared), calling convention, closure env, exception objects |

## The emission shape (all targets)

Per-node dispatch is a plain `std::visit` over `Overloaded` lambdas — no
virtual call per node. In `BackendLLVM`, `BodyEmitter` owns this walk and
receives a `FunctionFrame` (a per-body local, not a member of the backend
state) carrying the unit's `Values`/`Callees` tables and the current LLVM
insert point:

```cpp
std::visit( Meta::Overloaded{
    [&]( const Frontend::Call   &Node ) { EmitCall( Id, Node ); },
    [&]( const Frontend::Binary &Node ) { EmitBinary( Id, Node ); },  // CalleeEntry ? call : instruction
    [&]( const Frontend::Assign &Node ) { EmitAssign( Id, Node ); },
    ...
    [&]( const auto & )                 { /* non-value or inert node */ }
}, Unit.Ast->Expr( Id ) );
```

The backend never asks whether `10` is 32 or 64 bits, nor whether a call is
valid: it reads `Values`, `Callees` and the layouts, and emits. A hole in
those tables is a middle-end bug, reported as such — never guessed around.
