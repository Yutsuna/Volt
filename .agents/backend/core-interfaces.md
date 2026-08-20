# Backend spec: core interfaces — `BackendCore`

The target-agnostic layer every emitter builds on. Code lives in
`source/Volt/Backend/BackendCore/` (module `BackendCore`, DEPS `MiddleEnd`).

## The input contract, materialised

`rules/core-ast.md` promises a backend a 27-node core AST, fully typed, with
callee resolutions and closure frames precomputed. Each promise now has a
concrete read side:

| Promise | Where a backend reads it |
|---|---|
| 25 core nodes, no sugar | `Frontend::AstContext` (checked by `AstInvariant`) |
| every value expression typed | `MiddleEnd::TypeSystem::UnitTypes` (`Values.ExprType( Id )`) |
| method vs. machine instruction | `MiddleEnd::IR::UnitCallees` (`MiddleEnd/IR/CalleeMap.hpp`) |
| closure size/alignment/escape | `MiddleEnd::Resolver::SynthesizeClosureFrame( Scopes, Types, ScopeId )` |
| memory shape of a type | `MiddleEnd::TypeSystem::TypeStore` → `LayoutNode`, sized by `LayoutEngine` |

`UnitCallees` is the piece that had to be built for this spec: TypeChecker's
resolutions were pass-local and died with the pass. They are now snapshotted
once at the end of `TypeChecker` (order 30) into `CompileUnit::Callees` —
final state only, because inference refines entries in place. The
`CalleeEntry` also carries `Bindings` + `Receiver`, which is exactly what
monomorphisation needs to substitute into a generic body.

## `BackendInput` / `UnitView` — why not `CompileUnit`

`BackendCore` depends on `MiddleEnd`, never on `Driver`. A backend consumes
per-unit *facts* (AST, types, callees, scopes), not the Driver's
orchestration state, so `BackendInput.hpp` defines its own read-only view:

```cpp
struct UnitView    { std::uint32_t Ordinal; Module, Path, Ast*, Values*, Callees*, Scopes*, Synth* };
struct BackendInput{ MiddleEnd::TypeSystem::TypeStore *Types; std::span<const UnitView> Units; std::uint32_t StdlibUnitCount; };
```

The Driver (or a CLI command) maps each `CompileUnit` into a `UnitView`, in
**circuit link order** — dependencies first, entry module last — so a
single-pass emitter sees every callee's declaring unit before the call site's.
The dependency chain stays `Backend* → MiddleEnd → Frontend → Core`, and the
Driver may later grow a dependency on backends without a cycle.

`Synth` points at the `SynthesizedFunctions` table `ClosureLifting` built for
this unit: the synthesized top-level functions (closure bodies) that are not
TypeStore members and must be declared and defined as a separate sweep.
`StdlibUnitCount` is the count of leading `Units` entries that belong to the
standard library — used by `BackendLLVM` (and optionally by other targets) to
skip defining stdlib bodies when a precompiled artifact is linked instead.

### `Ordinal` — the bridge back to the store, and why it is a field

`Ordinal` is the **declaring-unit ordinal the `TypeStore` keys on**: the very
number `BindUnitTypes` stamped into `Member::Unit` and `NominalType::Unit`.
It is filled in `Driver::MakeBackendViews` from the *discovery* index.

It is deliberately **not** the view's own position in `Units`. The views are in
circuit link order while the ordinal is discovery order, and the two differ as
soon as a circuit has edges — so reconstructing one from the other is wrong in
exactly the cases that matter.

What it buys: a sweep can read the **store** — the build-wide resolved
interface — and still ask "which of these members does *this* unit hold a body
for", with a single `Member::Unit == UnitView::Ordinal` test. The alternative
(walk a `Decl` arena, search the store back by `DeclId`) is both slower and
unsound, because a `DeclId` is only meaningful inside the arena that minted it.
`BackendLLVM`'s declare and define sweeps are both built on this; the same
shape is what the VM and wasm emitters should use.

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

The spelling table is shared (`BackendCore/Instructions.inl` — one row per
family × operator, opcodes named in target-neutral enums) and each target maps
those enums onto its own encoding (`llvm.md`, `wasm.md`); the
*decision* is made once, upstream, by MiddleEnd. A backend that finds neither a
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
  `source/Volt/meson.build` (`subdir('Backend/...')`). `meson.build` configures
  `<MODULE>_EXPORT` (`rules/shared-lib-exports.md`); optional toolchains gate
  themselves with an option check (`BackendLLVM` ↔ `enable_llvm`).
- Instruction selection is a **manifest**, not a switch:
  `BackendCore/Instructions.inl` (one row per primitive family × operator →
  a target-neutral opcode enum) is re-included with different macro definitions
  to generate the lookup tables. A target adds its own enum→encoding mapping;
  it never re-lists the rows, and there is no `switch` over operators in any
  emitter (`rules/meta-first.md`). Any future wasm opcode table follows the
  same shape.
- **Two targets can share a middle.** `BackendLLVM` and `BackendJIT` both emit
  LLVM IR, so everything from core AST to a finished `llvm::Module` lives once
  in `BackendLlvmIr` and each adds only its tail. `BackendLlvmIr` organises its
  private sources by concern:
  `Private/Core/` (services, LLVM module context),
  `Private/Functions/` (declare/define sweeps, signature, parameter binder),
  `Private/Types/` (type mapping, ABI verifier),
  `Private/Lower/Expr/` (per-expression emitters),
  `Private/Lower/Stmt/` (statement emitters, tail value, loop),
  `Private/Lower/Closure/` (indirect call, block-next),
  `Private/Lower/Exception/` (raise/rescue/ensure, ancestry table),
  `Private/Lower/Mono/` (monomorphized body emitter).
  `BackendLLVM` keeps `Private/Target/` (optimizer, object emitter, IR emitter,
  linker driver, stdlib artifact builder); `BackendJIT` keeps the ORC layer.
  **`BackendJIT` must never include a `BackendLLVM` header** — the shared code
  is below both of them, never sideways between them.

## `IJitBackend` — the seam for a target that executes

`TargetBackend` describes a generator that produces an *artifact path*. A JIT
produces no file, so `BackendCore/ExecutableBackend.hpp` extends the runtime
seam: `IJitBackend : IBackend` adds `Run`, `Reload`, `EvalUnit` and
`LookupSymbol`. Those are virtual, which is within the rule — the ban is on a
virtual call **per node**, not per execution.

The Driver includes this header, never a backend one: `Driver::Run` and
`Driver::OpenReplSession` are where `volt run` / `volt repl` resolve to a
concrete backend, exactly as `Driver::Build` is for `--target`. See
`backend/jit.md`.
