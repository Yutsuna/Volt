# Backend spec: core interfaces — `BackendCore`

The target-agnostic layer every emitter builds on. Code lives in
`source/Volt/Backend/BackendCore/` (module `BackendCore`, DEPS `Sema`).

## The input contract, materialised

`rules/core-ast.md` promises a backend a 27-node core AST, fully typed, with
callee resolutions and closure frames precomputed. Each promise now has a
concrete read side:

| Promise | Where a backend reads it |
|---|---|
| 27 core nodes, no sugar | `Frontend::AstContext` (checked by `AstInvariant`) |
| every value expression typed | `Sema::UnitTypes` (`Values.ExprType( Id )`) |
| method vs. machine instruction | `Sema::UnitCallees` (`Layout/CalleeMap.hpp`) |
| closure size/alignment/escape | `Sema::SynthesizeClosureFrame( Scopes, Types, ScopeId )` |
| memory shape of a type | `Sema::TypeStore` → `LayoutNode`, sized by `LayoutEngine` |

`UnitCallees` is the piece that had to be built for this spec: TypeChecker's
resolutions were pass-local and died with the pass. They are now snapshotted
once at the end of `TypeChecker` (order 30) into `CompileUnit::Callees` —
final state only, because inference refines entries in place. The
`CalleeEntry` also carries `Bindings` + `Receiver`, which is exactly what
monomorphisation needs to substitute into a generic body.

## `BackendInput` / `UnitView` — why not `CompileUnit`

`BackendCore` depends on `Sema`, never on `Driver`. A backend consumes
per-unit *facts* (AST, types, callees, scopes), not the Driver's
orchestration state, so `BackendInput.hpp` defines its own read-only view:

```cpp
struct UnitView    { Module, Path, Ast*, Values*, Callees*, Scopes* };
struct BackendInput{ const TypeStore *Types; std::span<const UnitView> Units; };
```

The Driver (or a CLI command) maps each `CompileUnit` into a `UnitView`, in
**circuit link order** — dependencies first, entry module last — so a
single-pass emitter sees every callee's declaring unit before the call site's.
The dependency chain stays `Backend* → Sema → Frontend → Core`, and the
Driver may later grow a dependency on backends without a cycle.

## `concept TargetBackend` — the interface is compile-time

```cpp
template <typename T>
concept TargetBackend = requires ( T G, const BackendInput &I, const UnitView &U ) {
    { G.Name() }     -> std::convertible_to<std::string_view>;
    { G.Begin( I ) } -> std::same_as<void>;
    { G.EmitUnit( U ) } -> std::same_as<EEmitStatus>;
    { G.Finalize() } -> std::same_as<EmitResult>;
};
```

Emitters are concrete classes checked by `static_assert( TargetBackend<X> )`
in their own TU — breaking the contract is a compile error in the emitter,
not a runtime discovery. The **only** virtual seam is `IBackend` +
`TBackendAdapter<T>` (`MakeBackend`), used by the Driver to pick a target
from `--target` at runtime: one virtual hop **per unit**. Per-node dispatch
is never virtual:

```cpp
std::visit( Meta::Overloaded{
    [&]( const Frontend::Call &Node )    { EmitCall( Id, Node ); },
    [&]( const Frontend::Binary &Node )  { EmitBinary( Id, Node ); },
    ...
    [&]( const auto & )                  { /* non-value node */ } },
  Unit.Ast->Expr( Id ) );
```

## The operator/call protocol (one predicate, three backends)

For `Binary` / `Unary` / `Member` / free-function `Identifier`, every emitter
reads the same two-line protocol:

```
Callees->Get( CalleeId ) != nullptr, Decl != nullptr
    -> emit a call to that Member (its declaring unit + DeclId are on it)
otherwise
    -> a machine operation, selected from the receiver layout's
       Primitive{ Spelling, Bits }  ("i32" + "+"  ->  add, etc.)
```

The spelling table is per-target (`llvm.md`, `vm.md`, `wasm.md`) but the
*decision* is made once, upstream, by Sema. A backend that finds neither a
resolution nor a primitive layout has hit a middle-end bug — it must fail
loudly (`EEmitStatus::Error`), never guess. **Zero semantic analysis, zero
type inference in any backend.**

## `LayoutEngine` — the single ABI authority

`LayoutEngine( Store ).Of( LayoutId )` and `FieldOffset( LayoutId, Index )`
derive size/alignment/offsets from `LayoutNode` alone (see `abi.md` for the
rules). All three targets consume the same numbers; no emitter computes its
own offsets. No Volt type name enters the computation
(`rules/zero-hardcode.md`).

## `Monomorphizer` — instantiation is codegen work

Generic bodies are typed only after substitution (`UnitTypes::MarkDeferred`,
`rules/core-ast.md`). `Monomorphizer` is the shared work queue:

- A concrete use site (`Array<Int32>.push`) enqueues a `MonoRequest`
  — `NominalId` base + pre-order-flattened concrete argument tree, the
  cross-unit currency (`SemaTypeId` is per-unit and must not leak in).
- The emitter drains the queue: walk the generic declaration's body with the
  request's bindings substituted (the `CalleeEntry::Bindings` of the use
  site), emitting a specialised function per key.
- Keys dedupe globally, so `Array<Int32>` instantiates once per build and
  recursive generics terminate.

## Error policy

Everything semantic was refused loudly upstream (`rules/core-ast.md` lists
what and why). A backend therefore has exactly two failure modes: *not
implemented yet* (`Unimplemented`, honest skeleton state) and *middle-end
contract violation* (`Error`, a compiler bug). It never reports user-facing
diagnostics about Volt source.

## Module wiring & meta-first

- New backend = one directory under `source/Volt/Backend/` + one line in
  `cmake/VoltBuild.cmake`'s `VoltAddModules`. `VoltModule()` generates
  `<MODULE>_EXPORT` (`rules/shared-lib-exports.md`); optional toolchains gate
  themselves with an early `return()` (`BackendLLVM` ↔ `VOLT_ENABLE_LLVM`).
- Per-target instruction tables are **manifests**, not switches:
  `BackendVM/Bytecode.inl` (one `VOLT_OP` line per opcode → enum, name LUT,
  operand widths) is the template. A future LLVM spelling→instruction table
  or WASM opcode table follows the same shape (`rules/meta-first.md`).
