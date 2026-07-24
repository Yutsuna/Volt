# Backend spec: LLVM AOT — `volt build`

Native production builds: LLVM IR through `llvm::IRBuilder`, the standard
pass pipeline, object emission, native link. Module `BackendLLVM`
(`source/Volt/Backend/BackendLLVM/`), optional behind `VOLT_ENABLE_LLVM`
(the Nix package already carries `llvmPackages_latest`; the module links the
monolithic `LLVM` dylib when present). **No LLVM header escapes the module**:
`LlvmBackend` is pimpl'd, so the rest of the compiler never recompiles
against the LLVM API.

## Pipeline

```
BackendInput ─> declare pass ─> define pass ─> verify ─> optimize ─> emit .o ─> link
               (all units)      (all units,     (IR       (O0 dev /   (Target-   (mold/LLD)
                                 + mono queue)   verifier)  O2+LTO)     Machine)
```

Two sweeps over the units, both in circuit link order: **declare** creates an
`llvm::Function` for every reachable `def` (so forward references across
units resolve), **define** fills bodies by walking each function's stmt/expr
trees, draining the `Monomorphizer` queue as generic instantiations are
discovered. One `llvm::Module` per build to start (simplest correct thing;
per-unit modules + ThinLTO is a later optimisation with the same interface).

## Types: `LayoutNode` → `llvm::Type`

| Layout | LLVM type |
|---|---|
| `Primitive{ "i1", 1 }` | `i1` |
| `Primitive{ Spelling iN/uN, Bits }` | `iN` (signedness lives in the *instruction*, not the type) |
| `Primitive{ "f32"/"f64", Bits }` | `float` / `double` |
| `Primitive{ "ptr", 64 }` | `ptr` (opaque) |
| `Pointer{ Pointee }` | `ptr` (opaque pointers; pointee only informs GEP) |
| `Aggregate{ Fields }` | anonymous `%struct { ... }`, field order preserved |

The mapping reads **only** spelling + bits — the compiler never learns that
`"f64"` means `Float64` (`rules/zero-hardcode.md`). Aggregate offsets must
agree with `LayoutEngine` (`abi.md`); `DataLayout` is configured so natural
alignment matches, and a `static_assert`-style startup check compares the two
on every aggregate it emits (debug builds).

## The 27 nodes → IR

| Node | Emission |
|---|---|
| `IntLiteral` `FloatLiteral` `CharLiteral` `BoolLiteral` | `ConstantInt` / `ConstantFP` of the claimed type's primitive layout |
| `NilLiteral` | null of the claiming type's layout (`ptr` today) |
| `StringLiteral` | private constant bytes + the claiming type's aggregate `{ data, size }` |
| `SymbolLiteral` | interned u32 constant (interner table is a runtime concern, refused loudly for `to_string` — core-ast.md) |
| `ArrayLit` / `HashLit` | alloca/heap the claiming type's aggregate, then per-element stores / builder calls resolved by Sema |
| `Identifier` | local slot load (`alloca` + mem2reg) or free-function reference via `CalleeEntry` |
| `InstanceVar` | GEP on `self` at `LayoutEngine::FieldOffset` |
| `SelfExpr` / `SuperExpr` | first parameter of the method function / same, statically dispatched to the parent's method |
| `Member` | value position: GEP+load; callee position: never evaluated, consumed by `Call` via `CalleeEntry` |
| `Deref` | `load` of the pointee layout (`*p` — pointee = first generic arg of the claiming pointer type) |
| `Call` | direct `call` to the mangled symbol from `CalleeEntry::Decl`; `BlockArg` → closure param (below) |
| `Assign` | `store` (locals become allocas; mem2reg cleans up) |
| `Ternary` | `select` when both arms are trivially pure, otherwise two blocks + phi |
| `Binary` / `Unary` | protocol of `core-interfaces.md`: resolved → call; primitive → instruction table below |
| `CaseExpr` | flat `WhenClause{ Patterns: [i1], Body }` chain → conditional-branch ladder; all-constant integer patterns → `switch` (this is why CaseExpr stayed core) |
| `BeginExpr` / `RaiseExpr` | EH tiers below |
| `Lambda` / `Block` | closure emission below |
| `GenericInst` / `SizeOf` | inert: read `Values.Get( Id )` / the layout size as a constant; **never descend** |

Statements: `If`/`While` → basic blocks with the usual cond/body/merge shape;
`Return` → `ret`; `Break`/`Next` → branch to the loop's merge/latch;
`LocalDecl` → alloca in the entry block; `ExprStmt` → emit and drop.

### Primitive instruction table (spelling × operator)

Integer family (`i*`/`u*`/`ptr`): `add sub mul`, `sdiv/udiv` `srem/urem` by
the spelling's `i`/`u`, `icmp {s,u}{lt,gt,le,ge}` / `icmp eq ne`, `and or
xor shl {a,l}shr`. Float family (`f*`): `fadd fsub fmul fdiv`, `fcmp o*`.
`i1`: `and or xor`, `not` as `xor true` — the spelled operators `and`/`or`
short-circuit, so they emit as control flow, not instructions. Pointer `+`/`-`
(declared on the pointer nominal, heterogeneous — zero-hardcode.md) → `gep`.

## Closures — `ClosureEnvFrame` is already computed

`SynthesizeClosureFrame` hands the emitter `{ Fields[offset], TotalSize,
Alignment, bEscapes }`:

- Lambda body → a private function taking `ptr %env` as leading parameter.
- `bEscapes == false` (literal consumed at its call site): env is an
  `alloca` in the caller — zero heap.
- `bEscapes == true`: env is heap-allocated through the stdlib allocation
  entry point (an `@[External]`-annotated function, so the backend calls a
  symbol, not a hardcoded runtime).
- The closure *value* is the `{ ptr fn, ptr env }` pair aggregate; calling a
  `Block`/callable goes through `bApply` resolution like any call.

## Exceptions — `RaiseExpr` / `BeginExpr`, two tiers

Tier 1 (first implementation): **setjmp/sigsetjmp-free personality-less
unwinding is not attempted** — `begin/rescue` lowers to an out-parameter
error slot: `raise` stores the exception object (rooted at the
`@[ExceptionRoot]` nominal) into a thread-local slot and returns down a
poisoned path; `rescue` clauses test the slot's dynamic nominal id. Simple,
portable, correct, and identical semantics to the VM.
Tier 2 (once hot): Itanium zero-cost EH — `invoke` + `landingpad` with a
custom personality; the clause matching logic is unchanged, only the
transport differs. The choice is an emitter flag, not an AST concern.

## Optimisation & emission

- `PassBuilder` default pipelines: `-O0` + `-g` for `volt build` dev flavour,
  `-O2` (`-O3` opt-in) for release; full LTO only when `--lto`.
- `TargetMachine` from the host triple (cross later; the seam is one string).
- Emit ELF/Mach-O/COFF object via `addPassesToEmitFile`, then drive the
  system linker — mold when available, LLD fallback — exactly like the build
  system already prefers mold (`VoltOptions.cmake`).

## CLI

`volt build` (cli-surface.md): Driver runs the full pipeline, maps units to
`UnitView`s, `MakeBackend( LlvmBackend{} )`, `Begin → EmitUnit* → Finalize`,
then links `EmitResult::Artifact` into the output binary.
