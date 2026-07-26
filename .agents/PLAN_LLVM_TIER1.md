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
that are easy to get wrong.

*Amended during implementation.* Four things the plan did not anticipate, all
now recorded in `.agents/backend/llvm.md`:

- **Aggregates are addresses, everywhere.** `EmitExpr` on a struct-shaped
  expression yields a `ptr` at its storage, never a loaded struct. This makes
  GEP, `memcpy` and by-pointer parameter passing uniform, and it is why
  `FunctionFrame::Slots` holds `llvm::Value*` rather than `AllocaInst*`.
- **Implicit return is a *structural* tail rule**, not a typing one:
  `EmitStmts( List, bTail )`, where only a trailing `ExprStmt` (emit `ret`) and
  an `If` (recurse into both branches) read the flag. The stdlib depends on it
  — `Int8#<=>` ends in an `if/elsif/else` with no `return`. It asks nothing of
  Sema, which is the only reason it may live in a backend at all; a `Lowering`
  doing this would have to type the `Return`s it creates, which §"no pass after
  `TypeChecker`" forbids.
- **`Ternary` is always two blocks + phi.** The plan's "`select` when both arms
  are trivially pure" was dropped: both arms are arbitrary expressions, and
  `select` evaluates the one not taken. The optimiser recovers `select` where it
  is legal; the emitter must not decide purity.
- **Three refusals are genuine middle-end gaps, not deferrals** —
  `ArrayLit`/`HashLit` (no recorded construction protocol), `SizeOf` (no nominal
  for its operand), and a value-returning body falling off its end (no
  definite-return analysis, so it lowers to `unreachable`). Each names the hole.

Not yet exercised: nothing calls `Begin`/`EmitUnit` — `Driver` has
`MakeBackendViews` but no caller, and `Finalize` is still `Unimplemented`. The
first real run of both sweeps — and of the closures in phase 5 — arrives with
Phase 8 + Phase 9 (`--emit ir`). Everything through phase 5 therefore compiles
and is wired to itself, and has never executed.

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

- Lambda/Block body → a private `llvm::Function` with `ptr %env` **trailing** the
  declared parameters. *Corrected during implementation:* this line read
  "leading", which contradicted `abi.md`'s single ordering statement (`self`,
  declared params, `env`) and the signature `FunctionTypeOf` already writes.
  `abi.md` is the authority; the env trails.
- `bEscapes == false` → env is an `alloca` in the caller. Zero heap; this is the
  common case for a `do … end` consumed at its call site, and `ScopeTable::Escapes`
  already proves it.
- `bEscapes == true` → heap through the stdlib's `@[External]` allocator.
  *Refused during implementation:* Phase 0.1 records a C symbol **per member**,
  and nothing marks one member as *the* allocation entry point — so there is no
  allocator to call without the emitter naming `malloc` itself, which
  `rules/zero-hardcode.md` forbids. Reported by a message naming the hole. An
  escaping closure with **no captures** is unaffected: its env is null.
- The closure *value* is uniformly `{ ptr fn, ptr env }`; invoking one goes through
  `Member::bApply` resolution like any other call.

*Amended during implementation.* Four things the plan did not anticipate, all
recorded in `.agents/backend/llvm.md` and `abi.md`:

- **The pair is a layout, not an emitter-local shape.** The stdlib type
  claiming `FuncType`/`Lambda`/`Block` declares no field — `{ code, env }` is an
  ABI decision no Volt declaration can express — so a callable's *layout* was
  invalid, and a local holding one would have been zero bytes wide. It is
  materialised in `BackendCore::InstanceLayouts` (two `Pointer` fields, so the
  size follows the target's pointer size), which is also what will let the VM
  and wasm read a closure this backend wrote.
- **A capture is stored by address.** The frame gives every capture the same
  pointer-sized slot whatever its type; by-reference is the only reading that
  supports, and it makes a capture indistinguishable from a local inside the
  body — both are a place in `FunctionFrame::Slots`.
- **`next` in a block is a `ret`, not a branch** (and `break` there is a
  non-local exit, deferred to phase 6). `FunctionFrame::bClosure` tells the two
  senses of the keyword apart.
- **A closure body cannot reach `self`.** `ClosureEnvFrame` captures bindings,
  and a receiver is not one — a genuine upstream hole, reported as itself
  rather than as "outside a method".

Also done here, from the Verification section below: `tests/ZeroHardcode.cmake`
now scans `source/Volt/Backend` as well.

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

*Amended during implementation.* Two things the plan did not spell out, both
now recorded in `llvm.md`'s Exceptions section:

- **"Poisoned path" needed a concrete propagation rule.** A thread-local slot
  alone only tells a caller an exception happened; something has to make every
  frame between `raise` and the catching `begin` actually unwind. The rule:
  every non-`@[External]` call is followed by a check of the same slot, and
  finding one pending takes the identical poisoned path a `raise` does — a
  branch to this function's own innermost `begin` (`FunctionFrame::Rescues`,
  a stack shaped exactly like `Loops`) if it has one, otherwise an early
  return carrying no value. Reserves nothing in any signature; abi.md is
  unchanged.
- **`ensure` running on every path, without a raise inside a handler
  re-entering its own clause ladder, needed the `Rescues` target to change
  *twice* per `begin`** — `Dispatch` while the body runs, `Ensure` directly
  while a clause body runs (skipping re-matching, never skipping the
  `ensure`), and whatever was active outside this `begin` while `ensure`
  itself runs. "Unhandled" needed no separate signal: falling through every
  clause simply leaves the thread-local state as `Dispatch` found it, so a
  second check *after* `ensure` re-propagates it.

One upstream fix, not anticipated: `ExprInferencer`'s `RaiseExpr`/`BeginExpr`
case called `WriteLocal( Frontend::ExprId{}, ... )` for a rescue clause's bound
variable — `WriteLocal` resolves its site through `Scopes.BindingOf(Use)`, a
*use* → declaration index, and there is no use expression for a clause's own
binding, only its declaration, so the call silently never reached
`Values.SetSiteType`. Fixed to set the site directly (`BindingSite{ ClauseId }`),
exactly as `DeclStmtWalker`'s `LocalDecl` case already does, and — since the
backend needs the clause's resolved filter type regardless of whether it also
binds a name — moved outside the `VarName.IsValid()` guard so every clause
gets one, not just the ones with `as e`.

State: `volt-build llvm` clean under `-Werror`; 193/193 green; `volt-build llvm
debug asan` clean. `volt-build tidy` has one pre-existing failure in
`EmitTernary` (phase 4, untouched here) — a clang-analyzer false positive
against LLVM 22's own `PHINode`/`User` internals, reproduced identically on
the pre-phase-6 tree, not introduced by this phase. `graphify update .` run
(783 nodes, 1383 edges). Work left uncommitted in the tree, per prior phases.

Unchanged honest limit: still nothing calls `Begin`/`EmitUnit` — the exception
emitter compiles and is wired to itself, but has never executed. There is no
`samples/Codegen/` corpus yet to exercise `BeginRescue.vl`-shaped source
end-to-end; that arrives with phases 8 + 9.

---

## Phase 7 — Monomorphisation

`MonoEmitter.cpp` drains `Monomorphizer`. A concrete use site enqueues a `MonoRequest`
built from the callee's `Member` identity (`Owner` + `Name`) plus `CalleeEntry::Bindings`,
flattened to `NominalId`s (the cross-unit currency — `SemaTypeId` is per-unit and must
not leak into a key). A `UnitTypes`/`UnitCallees` slot is one-per-`ExprId`, but a
generic body's `ExprId`s are shared by every instantiation (`Array<Int32>` and
`Array<String>` reuse the same AST), so neither can hold more than one instantiation's
answer at a time.

Draining calls `Sema::ReinstantiateBody( Store, Ast, Scopes, Member, Owner, FlatArgs )`,
which re-runs the type checker's own expression inferencer over the member's declared
body with `self` and its generics bound to `FlatArgs` instead of the placeholder holes
the first, generic-shaped pass left, and returns a fresh `InstantiatedBody{ Values,
Callees }` — the same shapes a concrete unit publishes, just scoped to one instantiation.
This is monomorphisation's only semantic step, and it stays in Sema on purpose
(`rules/core-ast.md`: zero type inference in a backend) — a backend decides *when* to
instantiate, never *how* to type what it finds. `MonoEmitter` then walks the AST body
exactly as `DefineMember` walks a concrete one, reading every expression's type and
every call's resolution off the returned overlay rather than off the unit's own
`Values`/`Callees`.

Layouts come from Phase 0.3, keyed the same way. `Monomorphizer::Seen` dedupes
globally, so `Array<Int32>` instantiates once per build and recursive generics
terminate.

*Amended during implementation.* `MonoRequest` as originally sketched (`Base`
NominalId + `Args`) is exactly `InstanceLayouts`' own key — sufficient for a
*layout*, ambiguous for a *body*: `Array<Int32>` alone says nothing about
whether a request means `push`, `pop`, or `map`. Redesigned to
`{ Owner, Name, Args }`, `Owner`/`Name` naming the member the same way
`Member::Name` does.

The upstream addition the plan anticipated ("if substitution starts looking
like inference, push a `Reinstantiate` helper back into Sema") turned out to
be load-bearing rather than optional: a generic body's parameter types come
from the already-resolved `Member::Params` (`SigTypeId`) through the public
`Sema::Instantiate`, but *every other expression* in the body has no
resolved type at all inside the shared per-unit `UnitTypes` — not merely an
invalid one, an *absent* one, because `UnitTypes::OfExpr` is one slot per
`ExprId` and a generic body's `ExprId`s are shared by every instantiation
that ever calls it. `Sema::ReinstantiateBody` (`Sema/Layout/Instantiate.hpp`,
`Private/Passes/TypeChecker/Reinstantiate.cpp`) is that helper: it re-enters
the type checker's own `TrailingType`/`InferExpr`/`ConstrainExprType` over
one member's body with `self` and its generics bound to the request's
concrete `FlatArgs`, into a fresh `InstantiatedBody{ Values, Callees }`
scoped to just this instantiation. `FunctionFrame` grew two fields,
`Values`/`Callees`, read by every emitter function in place of
`Frame.Unit->Values`/`->Callees` — a concrete body sets them to its own
unit's, a monomorphised one to the request's overlay, and nothing downstream
of `EmitExpr`/`EmitStmt` can tell the two apart.

State: `volt-build llvm` clean under `-Werror`; 193/193 green; `volt-build
llvm tidy` clean except the same pre-existing `EmitTernary`/`EmitBinary`
clang-analyzer false positive against LLVM 22's own `CmpInst`/`User`
internals noted in phase 6, reproduced identically, not introduced here;
`volt-build asan llvm`, `ubsan llvm`, and `tsan llvm` all clean.
`graphify update source/Volt` run.

Unchanged honest limit: still nothing calls `Begin`/`EmitUnit`/`Finalize`
from a real CLI path — every sweep in this plan, monomorphisation included,
compiles and is wired to itself but has never executed against a generic
sample. There is no `samples/Codegen/` corpus yet to exercise a monomorphised
`Array<Int32>#push`-shaped instantiation end-to-end; that arrives with
phases 8 + 9, same as every phase before this one.

---

## Phase 8 — Optimise, emit, link

**Status: implemented, builds clean, 193/193 green. The inherited-default-method
gap this section used to call "Known-open" is now closed. End-to-end is blocked
on one further, deeper Sema gap found while pushing the harness past that point
— see "Known-open, not fixed here" below, which now names the real remaining
blocker. Not yet committed.**

`Finalize()` now does, in order:

1. `Impl->VerifyModule()` (`Optimizer.cpp`) — `llvm::verifyModule`, then
   `llvm::verifyFunction` per function to name the offending one. A failure is
   `EEmitStatus::Error`, never a Volt source diagnostic.
2. `Impl->RunOptimizationPipeline()` (`Optimizer.cpp`) — `PassBuilder` default
   pipelines: `buildO0DefaultPipeline` (still runs mem2reg, which the emitter
   depends on — it never builds SSA itself) at `OptLevel 0`, `buildPerModuleDefaultPipeline`
   at O1/O2/O3. `bLto` selects O3, not real cross-module LTO — there is only one
   `llvm::Module` per build so far, so "LTO" has nothing outside itself to link
   against yet; this is noted as a stub, honestly, in the code.
3. `Impl->EmitIrFile` / `Impl->EmitObjectFile` (`Optimizer.cpp`) — textual IR via
   `Module::print`, object via `TargetMachine::addPassesToEmitFile` (legacy
   `PassManager`, still the correct API on LLVM 22).
4. `Impl->LinkExecutable` (`Linker.cpp`) — finds a C driver (`cc`, then `clang`,
   then `gcc`) on `PATH` via `llvm::sys::findProgramByName` and shells out to it
   with `-fuse-ld=mold`/`-fuse-ld=lld` when present (mold preferred, matching
   `cmake/VoltOptions.cmake`), plus `-l<name>` for every distinct `@[External]`
   library the whole build names (`State::ExternLibraries`, walking the
   `TypeStore` the same way `DeclareAll` does). A C driver, not a raw `ld`
   invocation — it alone knows this host's CRT objects and default libc.
5. `EmitResult::Artifact` is the output path; `EmitOptions::Stage` (`Ir` /
   `Object` / `Link`) stops after steps 3/4/5 respectively. The intermediate
   object file for a `Link`-stage build is a real temp file
   (`llvm::sys::fs::createTemporaryFile` + `llvm::FileRemover`), not the final
   artifact.

**Upstream addition beyond the plan:** `EmitOptions`/`EEmitStage` were moved from
`LlvmState.hpp` (Private) into the public `LlvmEmitter.hpp`, and `LlvmBackend`
grew `void SetOptions( EmitOptions )`. Neither carries an LLVM type, so this
doesn't violate "no LLVM header escapes the module"; it exists because
`Finalize()` needs an output path and a stage *from somewhere*, and Phase 9
(`BuildCommand`) is what will call `SetOptions` from the parsed `-o`/`--emit`/`-O`
flags. Without it Phase 8 had no way to be driven at all.

### What actually happened when this ran for the first time

Nothing in this plan had ever called `Begin`/`EmitUnit`/`Finalize` before now —
every phase since 3 said so explicitly. Doing that for the first time (a small
throwaway C++ harness driving `Driver` + `LlvmBackend` directly, not the CLI,
which is still Phase 9) surfaced **four real, pre-existing bugs**, none of them
in this session's own Phase 8 code, all now fixed and covered by the existing
193-test suite staying green throughout:

1. **Missing `SEMA_EXPORT`.** `Sema::SynthesizeClosureFrame`
   (`Sema/Public/Volt/Sema/Layout/ClosureFrame.hpp`) had no export macro, so
   `libBackendLLVM_d.so` failed to load with an undefined symbol the moment
   anything outside the statically-linked `Volt_d` binary touched it — exactly
   the failure mode `rules/shared-lib-exports.md` predicts, just never
   triggered before because nothing had loaded this module standalone. Fixed
   with one `SEMA_EXPORT`.
2. **Stdlib bind order was filesystem-dependent.** `Driver::LoadStdLib` walked
   `source/Lib` with `fs::recursive_directory_iterator` and never sorted the
   result. `TypeBinder`'s Phase A binds one file at a time, so a field naming a
   type from a file the walk hadn't reached yet resolved to an invalid
   `LayoutId` — silently, since nothing before codegen ever reads
   `NominalType::Layout`. Mitigated by sorting `LoadStdLib`'s refs
   (`Driver.cpp`), and then fixed at the root (next point).
3. **`FunctionTypeOf` never resolved a `self`-typed parameter or return
   (`other : self`, `-> self` — `Arithmetic#min`/`#+`, `Comparable#<=>`, …).**
   `InstanceLayouts::OfSignature`/`FlattenSig` explicitly refuse `SigType::SelfParam`
   (that refusal is correct for a *field* typed `self`, which cannot exist, but
   wrong for a *method parameter*, which is the ordinary, load-bearing case
   `rules/zero-hardcode.md` itself uses as the canonical example). Fixed with a
   new `State::SignatureLayoutOf` in `LlvmEmitter.cpp` that special-cases
   `SelfParam` to `Instances.Of( Store, Owner, FlatArgs )` — the same
   computation `FunctionTypeOf` already does for the leading receiver.
4. **`TypeBinder`'s own two-phase contract ("declaring every name (phase A)
   must complete... before any signature is resolved (phase B)") was violated
   for *structural* layouts.** `Binder::BindType` (Phase A) called
   `AggregateOf`/`FieldLayoutOf` — a raw AST name lookup — inline, per type, as
   each file was bound, so a field naming an aggregate declared in a
   *different* file (`Exception#message : String`, `String` itself naming
   `Pointer<UInt8>`/`UInt64`) depended on nothing but stdlib file order,
   exactly like point 2 but one level deeper (struct-referencing-struct, not
   just struct-referencing-primitive — sorting alone cannot fix this in
   general). Fixed by adding a **third phase**, `Sema::ResolveStructLayouts`
   (`TypeBinder.hpp`/`.cpp`, called once from `Driver.cpp` right after Phase
   A's loop): a recursive, memoised, cross-unit resolver — `EnsureStructLayout`
   — that, given every unit's `AstContext*` indexed by discovery ordinal, jumps
   into whichever file actually declares a referenced type and resolves it on
   demand, depth-bounded (32) against a genuine cycle. This is a real,
   moderate-sized upstream change (new exported Sema function, new Driver call
   site), beyond "minimal" by the plan's own Phase-0 standard, but was load-bearing:
   without it, `DeclareAll` — which declares every concrete member of the
   *whole* stdlib, not just what a user program reaches — can never finish for
   any build, since `Exception` (stdlib-core, always present) always fails.

Also added, directly in `BackendLLVM` (not a Sema fix): `LlvmBackend::State::IsMixinOwner`.
A `mixin`'s own concrete (non-`abstract`) method — `Arithmetic#min`/`#max`,
`Bool#<=>` is not one of these, it is Bool's own — is generic over `self` in
exactly the sense a type parameter is (`other`'s type means "whichever type
includes me"), but declares zero `<T>`s of its own, so the existing
generic-owner exclusion (`Params.Size() > 0`) never caught it. `DeclareAll`/`DefineAll`
skip a mixin's own body outright (`IsMixinOwner`, checked via the declaring
unit's raw AST node kind, since `Struct`/`Class`/`Mixin`/`Enum` collapse to one
`NominalType` shape at bind time and carry no "is this a mixin" bit).

### Inherited-default-method monomorphisation — implemented this session

The previous revision of this section left this open; it is now closed.
`x.min( y )` on a concrete `Int32` resolves to `Arithmetic`'s own `Member`
(`Owner` ≠ the type that declares it), which used to be refused outright. It
is now routed through the same `Monomorphizer`/`Sema::ReinstantiateBody`
machinery Phase 7 built for generics — "a mixin's `self` is unresolved until
an including type is the receiver" is structurally identical to "a generic's
`T` is unresolved until a call site fixes it", and `ReinstantiateBody`'s
`Context.SelfType = Owner; Context.SelfValue = Self;` needed no change to
accept a concrete, non-generic `Owner`. Two changes, both small:

- `MonoEmitter.cpp`'s `LookupMonoMember` now calls `TypeStore::LookupMember`
  (own body → mixins → superclass, transitively) instead of scanning only
  `Store.Type( Owner ).Members` — the one-line reason the old refusal existed
  at all, since that scan could never find an inherited default.
- `ExprEmitter.cpp`'s `EmitResolvedCall` still computes `bOwnMember` (needed
  for `FunctionFor`'s mangling either way — `MangleFunction` keys on the
  *receiver* `Owner`, not the declaring mixin, so `Int32.min`/`Float64.min`
  already mangle to distinct symbols sharing one AST body), but now enqueues a
  `MonoRequest{ Owner, Entry.Decl->Name, FlatArgs }` whenever `Owner.IsValid()
  and not bOwnMember`, unconditionally on `FlatArgs` being non-empty (a
  concrete, non-generic receiver calling a non-generic mixin default has an
  empty `FlatArgs` — `Owner` alone is enough to key the request, unlike the
  pre-existing generic-owner branch, which still gates on `not
  FlatArgs.empty()`). No `Fail` path remains for this case.

Verified via a throwaway harness (see below): `Bool#<=>`, `Char#<=>`
(`self < other` / `self > other`, both resolving through `Comparable`'s
included operators onto `Arithmetic`-style bodies) now emit and define
correctly where they previously refused.

### A second real bug, found immediately after: `EmitTernary` and aggregates

With the refusal gone, the harness ran further and hit an **LLVM assertion**
(`PHINode::setIncomingValue`: "All operands to PHI node must be the same type
as the PHI node!"), in `Bool#to_string`'s `self ? "true" : "false"`. Root
cause: `EmitTernary` (`ExprEmitter.cpp`) built its merge-block `PHINode` from
`TypeOfExpr( Id )` — the *value* type, a `String` struct — but this file's own
stated ABI convention is that **an aggregate expression evaluates to a `ptr`
at its storage**, never to the struct value itself, so `Then`/`Else` (both
`String` literals) were already pointers. The PHI's declared type and its
incoming values disagreed by construction. Fixed by building the PHI's type
from `ParamTypeOfLayout( LayoutOfExpr( Id ) )` instead — the same
struct-to-`ptr` conversion `FunctionTypeOf` already uses for by-pointer
parameters — with the old `TypeOfExpr`-via-`Then->getType()` fallback kept
only for when the layout is unavailable. This was a **crash on well-typed
source**, not a diagnosable refusal, so it is also a genuine severity
regression relative to the "refuse loudly, never guess" standard the rest of
this codebase holds itself to; worth calling out explicitly since it slipped
past `AstInvariant`, `ZeroHardcode`, and all 193 existing tests (none of which
exercise a ternary over a non-primitive type).

### A third bug, in the stdlib itself: `Exception`'s `setter`

`source/Lib/Primitives/Exception.vl` declared `setter max_frames : Int32`.
`ParseFieldOrMember` (`Frontend/Private/Parser/ParseDecl.cpp`) only recognises
`getter`/`property` as accessor-prefix keywords — `setter` was never one, and
`EAccessor` (`Frontend/Public/Volt/Frontend/AST/Node.hpp`) has no `Setter`
value; nothing downstream (Sema, Backend) reads `Field::Accessor` at all, it
is inert metadata today. Consequently `setter` itself was consumed as the
*field's name*, leaving `max_frames : Int32` as unparsed trailing tokens the
field-parser silently dropped — the field that reached `TypeStore` was named
`setter`, with an invalid `DeclType`. This is exactly what
`EnsureStructLayout` reported as `"aggregate field 'setter' has no resolved
layout"` — correctly, on genuinely malformed input. `max_frames` is read only
via `@max_frames` inside `Exception` itself, never through a generated
accessor call anywhere in the tree, so the fix is the stdlib fix: dropped the
keyword, `max_frames : Int32` is a plain field. **Not a compiler bug** — a
`setter` keyword was never implemented, and this line predates that fact
being exercised by anything that reads memory layouts.

### Known-open, not fixed here: implicit (bare) local declarations have no storage identity

Pushing the harness past all three fixes above surfaced a fourth, and this one
is **not fixed this session** — it is architecturally larger than the other
three, spans Sema (not just `BackendLLVM`), and was only diagnosed, not
attempted, given its blast radius.

**The finding.** `Char#to_string`:
```
def to_string -> String
  buf = Pointer<UInt8>.malloc( 2_u64 )
  *( buf ) = self
  ...
```
`buf = Pointer<UInt8>.malloc( 2_u64 )` — no `: Type` — parses to a plain
`Assign` targeting a bare `Identifier`, **not** a `LocalDecl`
(`Frontend::Parser::ParseExprOrLocalStatement` only builds a `LocalDecl` for
`name : Type [= init]`; a colon-less `name = expr` cannot be told apart from a
reassignment at parse time, since that needs scope information the parser
does not have). `Sema::ScopeResolver` (order 10) only ever declares a
`BindingSite` for an explicit `LocalDecl`'s `StmtId` (or a `Param`); it has no
case for "an `Assign` target that does not already resolve", so `buf`'s first
occurrence is walked as an ordinary read, fails `Resolve`, and is counted as
merely `UnresolvedIdentifiers` — "not an error: may be a type name, a static
method, a member" reads the comment, and for `buf` none of those is true, but
ScopeResolver has no way to tell.

Type-checking still succeeds, because `TypeChecker`'s `Assign` case
(`ExprInferencer.cpp`) has its **own**, entirely separate fallback:
`TypeCheckerContext::FindLocal`/`WriteLocal` first try
`Ctx.Scopes.BindingOf( Use )` (works for a real `LocalDecl`/`Param`), and when
that is null — as here — fall back to `Locals[Name]`, a flat `Symbol →
SemaTypeId` map scoped only to the enclosing method, with **no stable
declaration site** (no `StmtId`/`ParamId`/`DeclId`, and `BindingSite`'s
variant has no fourth arm to hold one anyway). `Values.SetSiteType` is only
called in the `Bound`-found branch, so `buf` never enters `UnitTypes` by
`BindingSite` either — only by-`ExprId` typing, which `AstInvariant`'s
per-expression contract is satisfied by, so nothing upstream ever reports
this as wrong.

`BackendLLVM` has exactly one way to find a local's storage:
`Frame.Unit->Scopes->BindingOf( Id )`, i.e. `ScopeTable`'s published table —
and for `buf`, that is null on every one of its four occurrences (confirmed
by instrumenting `ScopeResolver` and `EmitAddress` for one run, then reverted
— no debug code remains in the tree). `EmitAddress` reports "an identifier
reached codegen with no scope binding" and stops, correctly, per its own
stated contract ("nothing here decides a type... a middle-end whose own
invariants say it cannot happen is worth a loud report, never a repair") —
this is the contract working as designed, not a new bug in `ExprEmitter.cpp`.

**Why this is the real blocker, not a corner case.** Bare `name = expr` (no
`: Type`) is the dominant local-variable style across `source/Lib/` and
`samples/` — `Array.vl`'s `new_cap = ...`, `String.vl`'s `min_size = ...`,
`Exception.vl`'s `effective_max = ...`, and so on. A typed `LocalDecl` is the
exception, not the rule. So this is not "one more stdlib method to fix" —
it is the reason **no nontrivial Volt program can reach a linked binary yet**,
independent of anything else in this plan.

**What a fix needs, sized but not attempted:**
1. `Sema::BindingSite` (`ScopeTable.hpp`) needs a fourth variant arm —
   `Frontend::ExprId` — to hold the first-occurrence's Target as a stable
   site, since neither a `StmtId` nor a `ParamId` names this declaration
   uniquely (a statement can contain more than one such Assign in principle,
   e.g. nested inside a call argument).
2. Every `std::visit`/`std::get_if` over `BindingSite` needs the new arm:
   `UnusedChecker.cpp`'s `LocOfSite`, and anywhere `.index()` is read
   positionally rather than through `std::visit`.
3. `ScopeResolver::WalkExpr` needs an `Assign` case (currently absent — it
   falls through to the generic `WalkFields`, which is exactly why the target
   Identifier is walked as an ordinary read): on a plain `=` (not a compound
   op — `AssignLowering`, order 24, runs after ScopeResolver and only handles
   `+=`-style anyway) whose `Target` is a bare `Identifier` that does not
   already `Resolve` in `Current`, declare it — `BindingSite{ Node.Target }` —
   instead of falling through, and self-bind that first occurrence too.
4. `TypeChecker`'s `WriteLocal`/`FindLocal` should then find a `Bound->Site`
   on the very first call, and the `Locals[Name]` name-only fallback becomes
   dead code for this path (kept for whatever case, if any, still needs it —
   worth re-auditing once (3) lands, not assumed).
5. `BackendLLVM`'s `EmitStmt`'s `Assign` handling needs to actually allocate a
   slot (`SlotFor`) the first time a `BindingSite` it has not seen is written,
   the same way `LocalDecl` already does — this repo has not been read closely
   enough this session to know whether that already falls out for free once
   (1)–(4) exist, or needs its own change; flagged, not guessed at.

This is real Sema surface, not a `BackendLLVM`-only fix, and is sized larger
than anything else attempted in this phase — it was diagnosed carefully
(traced with temporary instrumentation in `ScopeResolver.cpp`/`ExprEmitter.cpp`,
confirmed root cause, then **fully reverted**, no debug code left behind) but
deliberately not attempted blind, given how many call sites `BindingSite`
already has.

**Still not proven to run end-to-end.** The harness now gets past inherited
mixin defaults and past the ternary/aggregate PHI bug, and is blocked here
instead — further than either prior session got. No sample has yet produced a
linked, executed binary. `--emit ir`/`--emit obj` are exercised only as far as
this blocker allows; `Linker.cpp`'s `cc`/mold/lld invocation has still never
actually run to completion on real output. The throwaway harness
(`llvm_smoke.cpp`) lives only in the scratchpad, not the repo; Phase 9's
`BuildCommand` + a `samples/Codegen/` corpus remains the right place to make
this permanent and repeatable, per the plan's own Verification section.

### Files touched this phase

- `source/Volt/Backend/BackendLLVM/Private/Optimizer.cpp` (new)
- `source/Volt/Backend/BackendLLVM/Private/Linker.cpp` (new)
- `source/Volt/Backend/BackendLLVM/Public/Volt/BackendLLVM/LlvmEmitter.hpp` (`EmitOptions`/`EEmitStage` moved in, `SetOptions` added)
- `source/Volt/Backend/BackendLLVM/Private/LlvmState.hpp` (new `State` method declarations; `SignatureLayoutOf`, `IsMixinOwner`, the Phase 8 group)
- `source/Volt/Backend/BackendLLVM/Private/LlvmEmitter.cpp` (`Finalize()` rewritten; `SignatureLayoutOf`, `IsMixinOwner`; `DeclareAll`/`DefineAll` skip mixin owners)
- `source/Volt/Backend/BackendLLVM/Private/ExprEmitter.cpp` (`EmitResolvedCall` now enqueues an inherited-default `MonoRequest` instead of refusing; `EmitTernary`'s PHI type fixed to `ParamTypeOfLayout`)
- `source/Volt/Backend/BackendLLVM/Private/MonoEmitter.cpp` (`LookupMonoMember` now searches inherited members via `TypeStore::LookupMember`)
- `source/Volt/Sema/Public/Volt/Sema/Layout/ClosureFrame.hpp` (`SEMA_EXPORT`)
- `source/Volt/Sema/Public/Volt/Sema/Layout/TypeBinder.hpp` / `Private/Layout/TypeBinder.cpp` (new `ResolveStructLayouts` phase)
- `source/Volt/Driver/Private/Driver.cpp` (`LoadStdLib` sort; calls `ResolveStructLayouts`)
- `source/Lib/Primitives/Exception.vl` (dropped the unimplemented `setter` keyword)

State: `volt-build llvm debug test` clean under `-Werror`, 193/193 green,
throughout every fix above; `volt-build format` run, no changes beyond
formatting noise already tracked. `volt-build llvm tidy` **still not run to
completion** — run it before considering this phase closed. `graphify update .`
**not run this session** — do it before closing, per `rules/graphify.md`.
Nothing committed — this is all working-tree state, per `rules/` / memory ("no
autonomous commits").

### Next up

1. Design and implement the implicit-local `BindingSite` fix above (the real
   blocker now) — likely its own focused session given the number of call
   sites `BindingSite` already touches.
2. Run `volt-build llvm tidy` to completion and fix anything beyond the
   pre-existing `EmitTernary`/`EmitBinary` clang-analyzer false positive noted
   in phases 6–7.
3. Get one sample all the way through `Ir` → `Object` → `Link` → executed,
   confirming exit code/stdout — the acceptance test this plan has deferred
   since Phase 4. Blocked on (1).
4. `graphify update .`.
5. Phase 9 (`volt build` CLI): this is what turns the throwaway harness into a
   real, permanent, repeatable path (`tests/LlvmIr.cmake`, `tests/LlvmRun.cmake`,
   `samples/Codegen/`) — Phase 8 cannot be fully verified per this plan's own
   Verification section without it.

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
- ~~Extend `ZeroHardcode.cmake`'s grep to cover `source/Volt/Backend`~~ — done in
  phase 5; the emitter is exactly where a Volt type name is most tempting to
  hardcode, and the corpus is clean.

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
