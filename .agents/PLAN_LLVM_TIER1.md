# Implementation Plan — BackendLLVM: native AOT emitter for `volt build`

## Context

Volt's middle-end is finished and its output contract is specified and *mechanically
enforced*: `AstInvariant` (order 40) guarantees a 27-node core AST with no residual
sugar and a type on every value expression, `UnitCallees` snapshots the
method-vs-machine-instruction decision, `SynthesizeClosureFrame` precomputes every
closure environment, and `TypeStore`/`LayoutEngine` own the ABI. Nothing downstream
of that contract exists yet: `LlvmEmitter.cpp` is a 54-line skeleton that creates an
`LLVMContextRef` through the stable C API and returns `EEmitStatus::Unimplemented`,
and `volt build` is not even a registered CLI command.

The outcome this plan delivers: `volt build hello.vl -o hello` produces a linked
native executable, via LLVM IR built with `IRBuilder`, optimised through `PassBuilder`,
emitted as an object by `TargetMachine`, and linked by mold/LLD — with **zero semantic
analysis in the emitter**, per `.agents/backend/core-interfaces.md`.

Three decisions frame the work (confirmed):

- **Full road to a linked binary**, phased so each phase is independently green.
- **Minimal upstream additions are in scope** — the backend must never re-derive
  facts the middle-end already knows.
- **`VOLT_ENABLE_LLVM` stays `OFF` by default**; inside the module the `llvm-c`
  skeleton is replaced by the LLVM **C++** API, still behind the existing pimpl so no
  LLVM header escapes `BackendLLVM`.

---

## Architectural invariants this plan must not break

| Rule | Consequence for this work |
|---|---|
| `rules/zero-hardcode.md` | The emitter switches on `Primitive{ Spelling, Bits }` only. `"i32"`, `"f64"`, `"ptr"` are opaque strings; `Int32`/`String`/`Array` never appear in C++. |
| `rules/core-ast.md` | 27 node kinds, no more. A missing `Values`/`Callees` entry is `EEmitStatus::Error` (middle-end bug), never a guess. |
| `rules/meta-first.md` | The spelling×operator instruction table is a **manifest** (`Instructions.inl`), the shape `Bytecode.inl` already established — not a `switch`. |
| `rules/shared-lib-exports.md` | Anything crossing the `.so` boundary gets `BACKENDLLVM_EXPORT` / `BACKENDCORE_EXPORT`; find the set from mold's errors under `VOLT_BUILD_SHARED=ON`, not by guessing. |
| `rules/cpp-style.md` | C++26, Allman, `SpacesInParens`, PascalCase, `[[nodiscard]]`, `-Werror`. Free functions + `std::visit(Overloaded{…})` over virtuals. |
| `rules/cli-surface.md` | `volt build` options are already specified; `Main.cpp` stays a thin dispatcher. |

---

## Phase 0 — Upstream facts the emitter needs (Sema + Driver)

Small, surgical; each keeps a *decision* in the middle-end where it belongs.

**0.1 — `@[External]` symbol names.**
`source/Lib/Primitives/Pointer.vl` writes `@[External( "libc", "malloc" )]` and
`ParseDecl.cpp` sets `Method::bExternal`, but the **linked symbol name is recorded
nowhere**. Add to `Sema::Member` (`Sema/Public/Volt/Sema/Layout/TypeStore.hpp`):

```cpp
Symbol ExternLib;    // "libc"        — invalid when not @[External]
Symbol ExternSymbol; // "malloc"      — defaults to Name when the annotation omits it
```

Fill in `Layout/TypeBinder.cpp`'s `DeclareMembers` / `BindFunction`, in the same
`PendingAnnotation` loop that already handles `Primitive` / `Literal` /
`ExceptionRoot`. The emitter then declares an external `llvm::Function` from
`Member::ExternSymbol` without ever scanning sibling `Annotation` decls.

**0.2 — Driver exposes the build to a backend.**
`Driver::Units` is private and there is no accessor. Add to
`Driver/Public/Volt/Driver/Driver.hpp`:

```cpp
[[nodiscard]] std::size_t UnitCount () const;
[[nodiscard]] const CompileUnit &Unit ( std::size_t Index ) const;
// Units mapped to Backend::UnitView, ordered by CircuitGraph::TopoOrder
// (dependencies first, entry module last) — the order core-interfaces.md
// requires so a single-pass emitter sees every callee's unit first.
[[nodiscard]] std::vector<Backend::UnitView> MakeBackendViews () const;
```

`MakeBackendViews` reuses `CircuitGraph::TopoOrder` (`Driver/Private/CircuitGraph.cpp`),
already implemented and cycle-checked. This makes `Driver` depend on `BackendCore`;
the dependency chain `Driver → BackendCore → Sema` stays acyclic (`BackendCore` never
mentions `Driver`), which is exactly what `core-interfaces.md` anticipated.

**0.3 — Layouts for monomorphised instances.**
`TypeBinder::BindType` deliberately leaves `NominalType::Layout` invalid for generics
("only computable for a non-generic whose field types are already bound"). Codegen is
where `Array<Int32>` becomes concrete. Add to `BackendCore`:
`InstanceLayout.hpp` — `LayoutId LayoutOfInstance( TypeStore &, NominalId Base,
std::span<const NominalId> Args )`, memoised on the flattened `MonoRequest::Key()`,
which substitutes the argument nominals into the base's field `SigType`s and calls
`TypeStore::AddAggregate`. This is *materialisation*, not inference — no new type is
decided, only a memory shape derived from types Sema already fixed.

**0.4 — Symbol mangling (new, shared).**
No mangling infrastructure exists anywhere in the tree. Add
`BackendCore/Public/Volt/BackendCore/Mangler.hpp` + `Private/Mangler.cpp`:
`std::string MangleFunction( const TypeStore &, const Member &, std::span<const NominalId> Bindings )`.
Scheme (documented in the header, stable and demangler-friendly):
`_V<len><Module><len><Owner><len><Method>[ I <args…> E ]`. An `@[External]` member
bypasses mangling entirely and uses `ExternSymbol` verbatim — that is the whole point
of the C boundary. Shared by all three backends, so the VM's `FunctionTable` and the
wasm export table key on the same strings.

---

## Phase 1 — Module and toolchain: C++ API behind the pimpl

`source/Volt/Backend/BackendLLVM/CMakeLists.txt` already gates on `VOLT_ENABLE_LLVM`,
finds LLVM with `find_package(LLVM CONFIG)`, marks includes `SYSTEM` (keeping
`-Werror` off third-party headers), and prefers the monolithic `LLVM` dylib. Changes:

- Extend the `llvm_map_components_to_libnames` fallback list from `core support` to
  `core support irreader analysis passes target native codegen mc` (+ `lto` when
  `--lto` lands).
- Add `-fno-rtti` guarded by `LLVM_ENABLE_RTTI` — a stock LLVM is built without RTTI
  and mixing raises link errors that read as unrelated undefined vtables.
- `Private/` grows several TUs; the existing `SOURCES "Private/*.cpp"` glob covers them.

`LlvmEmitter.hpp` needs **no public change** — `LlvmBackend` already pimpls `State`
and satisfies `TargetBackend` (`static_assert` in the TU). Delete the `<llvm-c/Core.h>`
include and rebuild `State` on `llvm::LLVMContext`, `llvm::Module`,
`llvm::IRBuilder<>`, `llvm::TargetMachine`.

### Files under `BackendLLVM/Private/` (one concern each)

| File | Responsibility |
|---|---|
| `LlvmEmitter.cpp` | `LlvmBackend` lifecycle, `Begin`/`EmitUnit`/`Finalize`, the two sweeps |
| `LlvmState.hpp` | the pimpl'd `State`: context, module, builder, `TargetMachine`, caches, the `Monomorphizer`, the diagnostic sink |
| `TypeMapper.cpp` | `LayoutId → llvm::Type*`, memoised; the `DataLayout`-vs-`LayoutEngine` cross-check |
| `Instructions.inl` | **manifest**: one `VOLT_LLVM_OP( Family, Spelling, Operator, Opcode )` line per primitive operation |
| `ExprEmitter.cpp` | the 25 value-producing core nodes |
| `StmtEmitter.cpp` | `If` `While` `Return` `Break` `Next` `LocalDecl` `ExprStmt` |
| `ClosureEmitter.cpp` | `Lambda` / `Block` → private function + env |
| `ExceptionEmitter.cpp` | Tier-1 `RaiseExpr` / `BeginExpr` |
| `MonoEmitter.cpp` | drains the `Monomorphizer` queue |
| `Optimizer.cpp` | `PassBuilder` pipelines, verifier, `addPassesToEmitFile` |
| `Linker.cpp` | mold/LLD/`cc` driver invocation |

---

## Phase 2 — Types and the ABI cross-check

`TypeMapper.cpp` implements the table in `.agents/backend/llvm.md`, reading **only**
spelling and bits:

- `Primitive{ Spelling, Bits }` → `"f32"`/`"f64"` (first char `f`) → `getFloatTy`/
  `getDoubleTy`; `"ptr"` → opaque `ptr`; everything else → `getIntNTy( Bits )`.
  Signedness lives in the *instruction*, never the type.
- `Pointer{ … }` → opaque `ptr`; the pointee only informs GEP.
- `Aggregate{ Fields }` → anonymous `StructType::get`, declaration order preserved,
  never packed.

**The load-bearing detail:** configure the `llvm::DataLayout` from the `TargetMachine`
and, in debug builds, assert per emitted aggregate that
`DataLayout::getStructLayout(...)->getElementOffset( I )` equals
`LayoutEngine::FieldOffset( LayoutId, I )` and that the sizes agree. `LayoutEngine` is
the ABI authority (`abi.md`); a silent divergence here is the single worst failure
mode in the whole backend — it corrupts `@[External]` C structs with no diagnostic.
Cache `LayoutId → llvm::Type*` in a `DenseMap` on `State`.

---

## Phase 3 — Two sweeps over the units

`Begin( Input )` stores the `BackendInput`, creates the module and `TargetMachine`
from the host triple (`sys::getDefaultTargetTriple()` — the seam for cross-compilation
is that one string), and constructs the `Monomorphizer`.

**Declare sweep.** *Amended during implementation:* the sweep's input is the
**`TypeStore`, not the units' `Decl` arenas**. The store is the build-wide,
already-resolved interface of every unit, so one pass covers all units at once and
a `DeclId` — meaningful only inside the arena that minted it — never leaves its
unit. It runs in `Begin()` (which already holds every `UnitView`), not per
`EmitUnit`. Two consequences the store made visible:

- Iterating free functions needed `TypeStore::FreeFunctions()`; lookup-by-name
  answers "is this call resolvable", codegen asks "what must I emit a symbol for".
- `Pointer<Void>` is a signature whose *argument* names a type the stdlib never
  declares. `InstanceLayouts::OfSignature` therefore applies `Of()`'s own
  precedence — an attached layout wins over any substitution — *before*
  flattening, instead of failing on the unresolvable argument. Symmetrically, an
  unresolvable **result** is `void`: a `def` with no `-> T` has no return type
  (`rules/core-ast.md`), and an undeclared one is the same shape. An unresolvable
  **parameter** stays an error.

For each concrete `Method` and
free function, create an `llvm::Function` with the mangled name and the signature
derived from `Member::Params`/`Result` through `TypeMapper`. Parameter order is fixed
by `abi.md`, once, for all three targets: **`self`, declared params, `ptr %env`**.
Generic declarations are *skipped* — they have no signature until instantiated.
`@[External]` members get a declaration-only `Function` with the raw `ExternSymbol`
and C linkage. Doing this for all units first is what makes forward references across
units resolve without a second fixup pass.

**Define sweep.** *Amended during implementation:* symmetric with the declare
sweep, and for the same reason — it reads the **`TypeStore`**, keeping the members
whose `Member::Unit` equals this view's ordinal, rather than walking the unit's
`Decl` arena and searching the store back by `DeclId`.

That test needed one new fact, and it is the only upstream addition this phase
made: **`UnitView::Ordinal`** (`BackendCore/BackendInput.hpp`), filled from the
*discovery* index in `Driver::MakeBackendViews`. `Member::Unit` is discovery
order; `BackendInput::Units` is circuit *link* order; the two diverge as soon
as a circuit has edges, so the bridge has to be carried, not recomputed. Full
rationale in `.agents/backend/core-interfaces.md`.

Per function: entry block, then bind the parameters in `abi.md`'s order — read
here exactly as `FunctionTypeOf` wrote it, since the two are one contract.
A scalar parameter gets an `alloca` (so assigning to a parameter works, and
mem2reg undoes it); an **aggregate parameter is already its own slot**, arriving
as a pointer to the caller's storage, so it is registered directly. Then emit
the statement list. Instantiation requests discovered while walking land in the
`Monomorphizer` and are drained by `MonoEmitter` before `Finalize`.

The define sweep applies the declare sweep's exclusions plus one: an
`@[External]` member *has* a symbol — it is declared, and calls to it link —
but its body is outside Volt. `SymbolOf` is the single place that decides
between the C spelling verbatim and the mangled scheme, and both sweeps now go
through it (`DeclareMember` previously mangled `@[External]` names, which would
have emitted a call to `_V…malloc` instead of `malloc`).

**Dispatch shape**, verbatim from `core-interfaces.md` — per-node dispatch is never
virtual, and there is exactly one `std::visit` site per category:

```cpp
std::visit( Meta::Overloaded{
    [&]( const Frontend::Call &Node )   { return EmitCall( Id, Node ); },
    [&]( const Frontend::Binary &Node ) { return EmitBinary( Id, Node ); },
    ...
    [&]( const auto & )                 { return Fail( Id, "non-core node" ); } },
  Unit.Ast->Expr( Id ) );
```

---

## Phase 4 — The 27 nodes

Follows the mapping table in `.agents/backend/llvm.md` exactly. Notes on the parts
that are easy to get wrong:

**Literals.** The claiming type comes from `TypeStore::LookupNodeKind( "IntLiteral" )`
etc. — the compiler learns the width from *that type's* `Primitive` layout, never
from a default. `StringLiteral` → private constant byte array + the claiming
aggregate `{ data, size }` built from its own field offsets.
`ArrayLit`/`HashLit` → alloca/allocate the claiming aggregate, then per-element
stores; they stayed core precisely because they are already fully typed.

**Access.** `Identifier` → local `alloca` load via `ScopeTable::BindingOf( Id )`, or a
free-function reference through `CalleeEntry`. `InstanceVar` → GEP on `self` at
`LayoutEngine::FieldOffset`. `Deref` → `load` of the pointee, which is the first
generic argument of the type that claimed `PointerType` — never a hardcoded name.

**The operator protocol — the one that is easy to get wrong.** For `Binary`/`Unary`,
read `Unit.Callees->Get( CalleeId )` **first**:

```
entry && entry->Decl != nullptr   -> direct call to the mangled symbol
otherwise                         -> machine instruction from Primitive{ Spelling, Bits }
neither                           -> EEmitStatus::Error (middle-end bug)
```

The instruction side is `Instructions.inl`, a manifest keyed by
(family derived from the spelling's first char, operator token) → LLVM opcode:
integer `add sub mul`, `sdiv/udiv` and `srem/urem` chosen by the `i`/`u` prefix,
`icmp {s,u}{lt,gt,le,ge}` / `eq ne`, `and or xor shl {a,l}shr`; float `fadd fsub fmul
fdiv` + `fcmp o*`. Three exceptions handled outside the table: `i1` `and`/`or` are
**spelled** operators that short-circuit, so they emit as control flow, not
instructions; `not` is `xor true`; pointer `+`/`-` are heterogeneous (declared on the
pointer nominal) and emit `gep`.

**Control.** `If`/`While` → the usual cond/body/merge blocks; `Break`/`Next` → branch
to a merge/latch tracked on a small loop stack in `State`. `Ternary` → `select` when
both arms are trivially pure, otherwise two blocks + phi. `CaseExpr` after
`CaseLowering` is a flat `WhenClause{ Patterns:[i1], Body }` list → conditional-branch
ladder, upgraded to a `switch` when every pattern is a constant integer — that jump
table is the reason `CaseExpr` stayed core.

**Inert.** `GenericInst` and `SizeOf` carry no runtime value: read
`Values->ExprType( Id )` / the layout size as a constant and **never descend**.

---

## Phase 5 — Closures

`SynthesizeClosureFrame( *Unit.Scopes, *Unit.Values, ScopeId )` hands over
`{ Fields[Offset], TotalSize, Alignment, bEscapes }` — already computed, never
recomputed here.

- Lambda/Block body → a private `llvm::Function` with `ptr %env` as its **leading**
  parameter (`abi.md` ordering).
- `bEscapes == false` → env is an `alloca` in the caller. Zero heap; this is the
  common case for a `do … end` consumed at its call site, and `ScopeTable::Escapes`
  already proves it.
- `bEscapes == true` → heap through the stdlib's `@[External]` allocator (Phase 0.1
  gives the emitter its symbol) — "heap" is a linked symbol, not compiler behaviour.
- The closure *value* is uniformly `{ ptr fn, ptr env }`; invoking one goes through
  `Member::bApply` resolution like any other call.

---

## Phase 6 — Exceptions, Tier 1

Per `llvm.md`, the first implementation is the **error-slot** transport, semantically
identical to what the VM will do:

- `raise` stores the exception object (a pointer to an instance whose ancestry reaches
  `TypeStore::GetExceptionRoot()`) into a thread-local slot and returns down a poisoned
  path.
- `BeginExpr` emits the body, then per `RescueClause` tests the slot's dynamic
  `NominalId` against the clause's resolved nominal, using an **ancestry table emitted
  once per build as static data**. `EnsureBody` runs on both paths.

Tier 2 (Itanium `invoke`/`landingpad` + custom personality) is an emitter flag with
identical clause-matching logic — deliberately out of scope here, and explicitly not
an AST concern.

---

## Phase 7 — Monomorphisation

`MonoEmitter.cpp` drains `Monomorphizer`. A concrete use site enqueues a `MonoRequest`
built from `CalleeEntry::Bindings` + `Receiver`, mapped to `NominalId`s (the
cross-unit currency — `SemaTypeId` is per-unit and must not leak into a key). Draining
walks the generic declaration's body with those bindings substituted, resolving each
deferred expression's type through the substitution rather than through
`Values->ExprType` (which is correctly empty inside a generic body — `IsDeferred`
distinguishes "deferred until instantiation" from "the middle-end forgot"). Layouts
come from Phase 0.3. Keys dedupe globally, so `Array<Int32>` instantiates once per
build and recursive generics terminate.

---

## Phase 8 — Optimise, emit, link

`Finalize()`:

1. `llvm::verifyModule` — a failure here is `EEmitStatus::Error`, an emitter bug, and
   must print the offending function.
2. `PassBuilder` default pipelines: `O0` + `-g` for the dev flavour, `O2` (`O3`
   opt-in) for release, full LTO only under `--lto`.
3. `TargetMachine::addPassesToEmitFile` → ELF/Mach-O/COFF object.
4. `Linker.cpp` drives the system linker, **preferring mold then LLD** — the same
   preference `cmake/VoltOptions.cmake` already encodes for the compiler's own build —
   passing `-l<ExternLib>` for each distinct `@[External]` library named by the build.
5. `EmitResult::Artifact` is the output path; `--emit ir`/`--emit obj` stop after
   steps 2 and 3 respectively.

---

## Phase 9 — `volt build`

The CLI contract is already written (`rules/cli-surface.md`); only the implementation
is missing. Add, following `FCheckCommand` verbatim as the pattern
(`Volt/Public/Volt/CLI/Commands/CheckCommand.hpp` + its `Private/` TU, registered in
`CommandRegistry`):

- `Volt/Public/Volt/CLI/Commands/BuildCommand.hpp`
- `Volt/Private/Volt/CLI/Commands/BuildCommand.cpp`

Options exactly as specified: `-i/--input`, `-o/--output`, `--target native|wasm`,
`-O 0|2|3`, `--emit ir|obj`, `--lto`, `-h`. Flow: `Driver::CompileFiles` /
`CompileCircuit` → bail on `HasErrors()` → `Driver::MakeBackendViews()` →
`MakeBackend( Llvm::LlvmBackend{} )` → `Begin` → `EmitUnit` per unit → `Finalize` →
report. When `VOLT_ENABLE_LLVM` was off, the command is still registered and reports
a clean *"this build of volt was configured without LLVM"* — a missing toolchain must
never look like a compiler crash. `Main.cpp` stays untouched: registration is one
line, the command table is the manifest.

---

## Verification

Run at every phase boundary, not just at the end:

```sh
volt-build format test              # Allman/170-col + the existing suite
volt-build tidy                     # static analysis, -Werror clean
```

**Configure with LLVM on** (the module is off by default, so this is the only way the
backend is exercised):

```sh
volt-build clean debug              # with -DVOLT_ENABLE_LLVM=ON
```

**Shared-link check** — the failure mode `rules/shared-lib-exports.md` describes shows
up only at the final executable link, so build with `VOLT_BUILD_SHARED=ON` and fix the
exact symbols mold names, module by module. Do not pre-annotate by guessing.

**End-to-end, the acceptance test:**

```sh
./build/bin/Volt_d build samples/Sema/<smallest passing sample>.vl -o /tmp/out
/tmp/out ; echo "exit=$?"
./build/bin/Volt_d build <file> --emit ir -o /tmp/out.ll   # inspect IR by hand
```

**New test wiring** (`tests/`, alongside `GoldenTest.cmake` / `AstInvariant.cmake` /
`ZeroHardcode.cmake`):

- `tests/LlvmIr.cmake` — golden-compare `--emit ir` output for a corpus under a new
  `samples/Codegen/`, normalising away addresses and value numbering. Registered the
  same way the existing golden suite is, and skipped when LLVM is off.
- `tests/LlvmRun.cmake` — compile-and-run: build each `samples/Codegen/*.vl` to a
  binary, execute it, compare stdout + exit code against a `.expected` file. This is
  the only test that proves the ABI cross-check, closures and EH actually work.
- Extend `ZeroHardcode.cmake`'s grep to cover `source/Volt/Backend` — the emitter is
  exactly where a Volt type name is most tempting to hardcode.

**ASAN**, since the emitter allocates heavily and holds LLVM references across
arena reads:

```sh
volt-build debug asan               # on a samples/Codegen file
```

**Finally**, per `rules/graphify.md` — this adds modules, files and a manifest:

```sh
graphify update .
```

---

## Risks worth stating up front

- **`LayoutEngine` vs `DataLayout` divergence** is silent and corrupts the C
  boundary. The debug-build assertion in Phase 2 is not optional polish; it is the
  mitigation.
- **Monomorphisation is the largest single phase** and the one place where the "no
  type inference in the backend" rule is under real pressure. If substitution starts
  looking like inference, that is the signal to push a `Reinstantiate` helper back
  into Sema rather than to improvise in the emitter.
- **`rules/core-ast.md` lists known middle-end gaps** — no integer literal suffixes
  (`0_u64` types as the `IntLiteral` claimant), no return-type inference, several
  `samples/Syntax/**` fixtures that do not `check`. The backend will surface the first
  as "wrong width constant". These are pre-existing and out of scope; the correct
  response is a loud report naming the gap, never a widening guess in the emitter.
