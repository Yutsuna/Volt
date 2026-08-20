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
                     +---> [1] volt run / repl        ---> BackendJIT   (LLVM ORC, hot reload)
Middle-end (core AST)-+---> [2] volt build --target wasm -> BackendWASM (self-contained .wasm encoder)
                     +---> [3] volt build              ---> BackendLLVM (LLVM IR, native AOT)
```

All three sit on the shared **BackendCore** layer and are ordinary Volt
modules under `source/Volt/Backend/`.

Targets [1] and [3] are both LLVM backends, so everything between the core AST
and a finished `llvm::Module` lives once, in **`BackendLlvmIr`**, and each of
them adds only its own tail — object file plus linker for `BackendLLVM`, ORC
plus in-process execution for `BackendJIT`:

```
BackendCore ── BackendLlvmIr ─┬─ BackendLLVM   volt build
 (no LLVM)     AST -> Module  └─ BackendJIT    volt run / repl
      └─────── BackendWASM                     volt build --target wasm
```

`BackendCore` stays LLVM-free so `BackendWASM` does not inherit an LLVM link,
and `BackendJIT` never depends on `BackendLLVM`.

## The spec

| Document | Covers |
|---|---|
| [`backend/core-interfaces.md`](backend/core-interfaces.md) | `concept TargetBackend`, `BackendInput`/`UnitView`, node dispatch, the callee protocol, `Monomorphizer`, error policy, module wiring |
| [`backend/llvm.md`](backend/llvm.md) | 24-node → IR mapping, emitter architecture (service decomposition), instruction table, control flow, closures, EH tiers, optimisation & link pipeline |
| [`backend/jit.md`](backend/jit.md) | ORC layering, pre-compiled stdlib `.so`, the TLS accessor, module granularity, **hot reload** via the indirection seam, REPL sessions |
| [`backend/wasm.md`](backend/wasm.md) | module sections, layout → wasm types, JS bridge imports, the JSX dependency, `--target wasm` |
| [`backend/abi.md`](backend/abi.md) | `LayoutEngine` rules, object/string/array layout (stdlib-declared), calling convention, closure env, exception objects |

The bytecode VM that used to hold target [1] is retired; `PLAN_BACKEND_JIT.md`
records why, and `backend/jit.md` carries over the two designs worth keeping
from it — the `FunctionTable` indirection seam and the REPL session model.

## The emission shape (all targets)

Per-node dispatch is a plain `std::visit` over `Overloaded` lambdas — no
virtual call per node. In `BackendLlvmIr`, `BodyEmitter` owns this walk and
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
