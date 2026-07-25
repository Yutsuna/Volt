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

Two sweeps, **both driven by the `TypeStore`, not by the units' `Decl`
arenas**: the store is the build-wide, already-resolved interface of every
unit, so a `DeclId` — meaningful only inside the arena that minted it — never
leaves its unit.

- **declare** (`DeclareAll`, once, in `Begin()`) creates an `llvm::Function`
  for every concrete member of the whole build, so forward references across
  units resolve with no fixup pass.
- **define** (`DefineAll`, once per `EmitUnit`, in circuit link order) sweeps
  the *same* store and keeps only the members this unit holds a body for,
  draining the `Monomorphizer` queue as instantiations are discovered.

The two sweeps apply the same exclusions, in the same order — not a method,
`abstract`, `OwnGenerics > 0` — and `define` adds one: an `@[External]` member
*has* a symbol (it is declared, and calls to it link) but its body lives
outside Volt.

"Which unit holds this body" is answered by **`Member::Unit == UnitView::Ordinal`**.
That ordinal is the declaring unit's *discovery* index — the one `BindUnitTypes`
stamped on every `Member` and `NominalType` — and deliberately **not** the
view's position in `BackendInput::Units`, which is circuit *link* order. The
two diverge as soon as a circuit has edges, so the bridge is an explicit field
rather than an index (see `core-interfaces.md`).

One `llvm::Module` per build to start (simplest correct thing; per-unit
modules + ThinLTO is a later optimisation with the same interface).

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
| `ArrayLit` / `HashLit` | **refused** — see "middle-end gaps found" below |
| `Identifier` | local slot load (`alloca` + mem2reg) or free-function reference via `CalleeEntry` |
| `InstanceVar` | GEP on `self` at `LayoutEngine::FieldOffset` |
| `SelfExpr` / `SuperExpr` | first parameter of the method function / same, statically dispatched to the parent's method |
| `Member` | value position: GEP+load; callee position: never evaluated, consumed by `Call` via `CalleeEntry` |
| `Deref` | `load` of the pointee layout (`*p` — pointee = first generic arg of the claiming pointer type) |
| `Call` | direct `call` to the mangled symbol from `CalleeEntry::Decl`; `BlockArg` → closure param (below) |
| `Assign` | `store` (locals become allocas; mem2reg cleans up) |
| `Ternary` | always two blocks + phi — **never `select`**, which evaluates the arm not taken, and both arms are arbitrary expressions |
| `Binary` / `Unary` | protocol of `core-interfaces.md`: resolved → call; primitive → instruction table below |
| `CaseExpr` | flat `WhenClause{ Patterns: [i1], Body }` chain → conditional-branch ladder; all-constant integer patterns → `switch` (this is why CaseExpr stayed core) |
| `BeginExpr` / `RaiseExpr` | EH tiers below |
| `Lambda` / `Block` | closure emission below |
| `GenericInst` / `SizeOf` | inert: read `Values.Get( Id )` / the layout size as a constant; **never descend** |

Statements: `If`/`While` → basic blocks with the usual cond/body/merge shape;
`Return` → `ret`; `Break`/`Next` → branch to the loop's merge/latch;
`LocalDecl` → alloca in the entry block; `ExprStmt` → emit and drop.

### Two conventions the whole emitter rests on

1. **A scalar is a register; an aggregate is an address.** `EmitExpr` on a
   struct-shaped expression hands back a `ptr` at its storage, never a loaded
   struct value. This is `abi.md`'s "aggregates by pointer" applied one level
   down, and it makes GEP, `memcpy` and by-pointer parameter passing uniform:
   `EmitStore` is a plain `store` for a scalar and a `CreateMemCpy` sized by
   `LayoutEngine` for an aggregate, with no third case anywhere.

   Its one visible consequence: an **aggregate parameter is already its own
   slot** — it arrives as a pointer to the caller's storage — so
   `FunctionFrame::Slots` maps a `BindingSite` to an `llvm::Value*`, not an
   `AllocaInst*`. Copying such a parameter into an `alloca` would only add a
   `memcpy` nothing reads.

2. **Every `alloca` goes in the entry block**, whatever block the walk is in
   when it needs one (`MakeTemp`). That is mem2reg's precondition, and it is
   the whole reason the emitter never constructs SSA itself.

### The tail rule — implicit return, decided structurally

Volt does not require `return`: `Int8#<=>` ends in an `if/elsif/else` chain and
its value is the value of whichever branch ran. `EmitStmts( List, bTail )`
marks the last statement of a list as being in result position, and exactly two
node kinds read that flag:

- a trailing `ExprStmt` emits `ret` instead of dropping its value;
- an `If` passes it on to **both** branches — an `elsif` chain is a nested `If`
  in the `Else` branch (`Stmt.hpp`), so propagation is what makes the whole
  chain a result.

Nothing else propagates it, because no other node's value can be the
function's. The rule is **positional, not typed**: it asks nothing of Sema,
which is the only reason it is allowed to live in a backend at all. `EmitCase`
uses the same rule per clause, converging through a slot rather than a phi
(a clause is a statement list, so the incoming-block count is not known until
the ladder is built).

Arguably this belongs in a `Lowering` — but that pass would have to type the
`Return` nodes it creates, which is exactly what `rules/core-ast.md`'s
structural invariant forbids after `TypeChecker`. Positional emission is the
cheaper correct answer.

### Primitive instruction table (spelling × operator)

`Instructions.inl` is the manifest (`rules/meta-first.md`), three macros wide:

```
VOLT_LLVM_BINOP( Family, Token, Opcode )     llvm::Instruction::BinaryOps
VOLT_LLVM_CMP( Family, Token, Predicate )    llvm::CmpInst::Predicate
VOLT_LLVM_UNOP( Family, Token, Kind )        EUnaryOp
```

Three families, each derived from **one character** of the opaque spelling —
`SInt` for `i*`, `UInt` for `u*`, `Float` for `f*`, with `"ptr"` joining the
unsigned integers (an address has no sign bit, so `p < q` is `icmp ult`). The
signed/unsigned split is written only where the machine genuinely has two
instructions (`sdiv`/`udiv`, `icmp slt`/`icmp ult`); `add` is one row per
family rather than a conditional.

Adding an operator is one line; adding a family is one `EOpFamily` row plus its
lines. Four things are deliberately *not* rows, because none is a table lookup:

- `and` / `or` (and `&&` / `||`) **short-circuit** → two blocks + phi;
- `not` / `!` needs the operand's width to build `xor true` → `EUnaryOp::LogicalNot`;
- pointer `+` / `-` are heterogeneous (`( offset : UInt64 ) -> Pointer<T>`,
  declared on the pointer nominal itself) → a `gep` whose stride is the
  pointee's size, the pointee being the receiver's first generic argument;
- an operator `UnitCallees` **resolved to a method** never reaches the table at
  all. `Callees->Get( Id )` is consulted *first*, exactly as
  `rules/core-ast.md` specifies, and `Binary`/`Unary`/`Call` then share one
  emission path (`EmitResolvedCall`) — the three differ only in where the
  receiver and the operands come from.

## Middle-end gaps this backend surfaced

Each is refused by a message naming the hole rather than guessed at, per
`core-interfaces.md`. None is a regression; all are genuine missing facts.

- **`ArrayLit` / `HashLit` have no recorded construction protocol.** They are
  fully typed, so they stayed core — but filling `{ data, size }` means
  allocating a backing buffer and knowing which field holds what, and neither
  is written down anywhere a backend may read. Inventing a field-order
  convention would silently corrupt any other shape, so it is reported instead.
- **`SizeOf` records no nominal for its operand.** The node is inert by
  contract ("read the layout size and never descend"), but nothing links the
  operand to a `NominalId`, so there is no layout to size.
- **A value-returning body can fall off its end.** Volt has no definite-return
  analysis — an `if` with no `else` in tail position is accepted — so this path
  is reachable from valid source and cannot be a hard failure. It lowers to
  `unreachable`, which is the honest reading of "the middle-end promised
  control never gets here".
- **Integer literal suffixes are parsed and ignored** (already recorded in
  `rules/core-ast.md`). The decoder trims the suffix and takes its width from
  the *layout*, always — honouring the suffix here would make the backend
  disagree with the type Sema assigned, which is worse than the known gap.

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
