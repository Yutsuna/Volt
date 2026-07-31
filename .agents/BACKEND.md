# Backend Architecture — Declarative Code Generation & Targets

The Backend consumes the middle-end's output and emits code. Its philosophy
is **declarative generation by pattern matching**:

- **Zero semantic analysis in the Backend.**
- **Zero type inference or resolution in the Backend.**
- **Zero uncertainty** — anything unclear was refused loudly upstream.

> **Input contract: [`rules/core-ast.md`](rules/core-ast.md).** Exactly
> **25 core nodes** (the 11 sugar nodes are gone, checked by `AstInvariant`),
> every value expression typed — outright in concrete code, after
> substitution inside a generic body. The contract is *materialised* for
> backends as: `UnitTypes` (expression types), `UnitCallees`
> (`Sema/Layout/CalleeMap.hpp` — the method-vs-machine-instruction protocol,
> snapshotted per unit at the end of TypeChecker), `SynthesizeClosureFrame`
> (env size/alignment/`bEscapes`), and the `TypeStore`'s `MemoryLayout`s.

## The 3 targets

```
                     ┌───> [1] volt run / repl        ───> BackendVM    (bytecode VM, hot reload)
Middle-end (core AST)─┼───> [2] volt build --target wasm ─> BackendWASM (self-contained .wasm encoder)
                     └───> [3] volt build              ───> BackendLLVM (LLVM IR, native AOT)
```

All three sit on the shared **BackendCore** layer and are ordinary Volt
modules under `source/Volt/Backend/`.

## The spec

| Document | Covers |
|---|---|
| [`backend/core-interfaces.md`](backend/core-interfaces.md) | `concept TargetBackend`, `BackendInput`/`UnitView`, node dispatch, the callee protocol, `Monomorphizer`, error policy, module wiring |
| [`backend/llvm.md`](backend/llvm.md) | 27-node → IR mapping, instruction table, control flow, closures, EH tiers, optimisation & link pipeline |
| [`backend/vm.md`](backend/vm.md) | bytecode ISA (`Bytecode.inl` manifest), frames, dispatch, **hot reload** via the FunctionTable seam, REPL |
| [`backend/wasm.md`](backend/wasm.md) | module sections, layout → wasm types, JS bridge imports, the JSX dependency, `--target wasm` |
| [`backend/abi.md`](backend/abi.md) | `LayoutEngine` rules, object/string/array layout (stdlib-declared), calling convention, closure env, exception objects |

## The emission shape (all targets)

```cpp
std::visit( Meta::Overloaded{
    [&]( const Frontend::Call &Node )   { EmitCall( Id, Node ); },
    [&]( const Frontend::Binary &Node ) { EmitBinary( Id, Node ); },   // CalleeEntry ? call : instruction
    [&]( const Frontend::Lambda &Node ) { EmitClosure( Id, Node ); },  // ClosureEnvFrame precomputed
    ...
}, Unit.Ast->Expr( Id ) );
```

The backend never asks whether `10` is 32 or 64 bits, nor whether a call is
valid: it reads `Values`, `Callees` and the layouts, and emits. A hole in
those tables is a middle-end bug, reported as such — never guessed around.
