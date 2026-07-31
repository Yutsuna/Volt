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

   **`ptr` is not a proxy for "aggregate", and asking the LLVM type is the
   wrong question.** `@[Primitive( "ptr", 64 )]` — every `Pointer<T>` — maps
   to `ptr` as well, and it is a *scalar*: it arrives as a bare value and needs
   an `alloca` like any other. All three parameter lists (`DefineMember`,
   `EmitMonomorphizedBody`, `EmitClosureBody`) used to test
   `Arg->getType()->isPointerTy()`, so every pointer parameter was bound as its
   own slot and every read of the name loaded *through* it. That is
   `String.from_c_string( p )` handing `strlen` whatever `*p` held — the
   crash under every `raise`, since `Exception#initialize` captures a
   backtrace. They now share one binder, `BindParameter`, whose `bByAddress`
   is the *same* answer `ParamTypeOfLayout` gave when it built the signature:
   `IsAggregate( layout )`, plus `Member::ParamIsBlock`. Regression sample:
   `samples/Codegen/PointerParam.vl`.

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

**A tail that has no value stores nothing.** `begin` and `case` converge their
arms through a slot rather than a phi, and an arm's tail expression can
perfectly well be a call to a `-> Void` member — `begin level3() rescue e : E
then 7 end` in statement position is valid Volt where only the rescue arm has a
value. Both go through one helper, `StoreTailValue`, which skips a void operand
and *fails loudly* on any other type disagreement, since that would mean the
arms were typed inconsistently. Not a stylistic guard: handing `CreateStore` a
void value builds an ill-formed instruction, and LLVM answers by asking its
`DataLayout` for the alignment of a type that has none — which is not a
diagnostic but an unbounded scan inside the library. It hung the compiler
outright on a fifteen-line program.

### String and char literals: escapes decode where bytes are made

The lexer interns the **source spelling** — it steps over `\n` without
decoding it, because `volt parse` and the golden fixtures show source text.
Decoding therefore belongs where bytes are actually materialised, which is
`EmitStringLiteral` / `EmitCharLiteral`, and both read one alphabet
(`DecodeEscape`: `\n \t \r \0 \e \\ \' \"`) so two tables cannot disagree. The
decoded length is what the aggregate's size field carries — `"a\n"` is two
bytes, not three — and an unrecognised escape keeps both characters rather
than being refused, since the lexer already accepted the literal and a backend
does not diagnose Volt source. Before this, every `\n` in every Volt string
reached the program as a literal backslash-n.

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
- ~~**`SizeOf` records no nominal for its operand.**~~ **Closed.** The node is
  inert by contract ("read the layout size and never descend"), and the missing
  half was the link from the operand to a type. `TypeChecker` now resolves the
  written annotation and publishes the result on the node's *own site*
  (`UnitTypes::SetSiteType( BindingSite{ Id } )` — the channel a `rescue`
  clause's filter already uses for "a type attached to an Id that is not a
  value expression"). The emitter measures that layout through `LayoutEngine`
  and takes the constant's width from the use site, exactly as an integer
  literal does. Inside a generic body `sizeof T` still defers; a
  *re-instantiation* answers it, because `Sema::ReinstantiateBody` now binds
  the owner's parameter names to the request's concrete arguments
  (`UnitSink::Bindings`) — which is what `Pointer<T>#malloc` needs to compute
  `count * sizeof T` at all.
- **A value-returning body can fall off its end.** Volt has no definite-return
  analysis — an `if` with no `else` in tail position is accepted — so this path
  is reachable from valid source and cannot be a hard failure. It lowers to
  `unreachable`, which is the honest reading of "the middle-end promised
  control never gets here".
- **No allocation entry point is marked, so an escaping environment cannot be
  allocated.** `bEscapes == true` with at least one capture needs a heap env,
  and `abi.md` is explicit that "heap" means a call to the stdlib's annotated
  allocator — a linked symbol, not compiler behaviour. `@[External]` records a
  C symbol *per member*, but nothing marks one member as *the* allocator, so
  the emitter would have to name `malloc` itself, which
  `rules/zero-hardcode.md` forbids. Refused by a message naming the hole. An
  escaping closure that captures **nothing** is unaffected: its env is null.
  The fix is upstream and small — one annotation (`@[Allocator]`, say) read in
  the same `PendingAnnotation` loop that already handles `@[External]`.
- **A closure body cannot reach `self`.** `ClosureEnvFrame` captures
  *bindings*, and a receiver is not one, so a `do … end` inside a method that
  touches `self` or `@x` has nothing to reach it through. Reported as that,
  not as "outside a method" — `Frame.bClosure` is what distinguishes the two
  messages. The fix is upstream: record the receiver as a capture, or add a
  `bCapturesSelf` to the frame.
- ~~**`abi.md` fixes how an aggregate travels *into* a call, not out of one.**~~
  **Decided: by value, spilled on arrival.** A returned aggregate keeps the
  struct type `FunctionTypeOf` already gave it, and the two conversion points
  are the only places a value leaves or enters its slot: `CoerceWidth` loads
  the struct at a `ret`, and `EmitResolvedCall` stores the returned struct into
  a fresh slot of this frame. Everything between them still obeys "an aggregate
  expression evaluates to a `ptr` at its storage". By value rather than sret
  because the callee's storage does not outlive it — the spill is what makes
  the result the caller's — and because it leaves every signature unchanged.
  Recorded in `abi.md`.
- **Integer literal suffixes are parsed and ignored** (already recorded in
  `rules/core-ast.md`). The decoder trims the suffix and takes its width from
  the *layout*, always — honouring the suffix here would make the backend
  disagree with the type Sema assigned, which is worse than the known gap.
- **A use's `ExprType` can lag behind its binding's `SiteType`.** A local with
  no annotation settles late: `ConstrainNode( Identifier )` moves the site when
  some later context finally says what the type is, but the uses *already*
  inferred against the provisional type keep it. `i = 0_u64` in
  `Exception#format_backtrace` leaves those uses reading `Int32` while the site
  is correctly `UInt64`. Nothing is mis-emitted — the slot comes from
  `SiteType`, which is right — but it fixes which of the two a store may
  believe: **`EmitStore` takes its width from the destination itself**
  (`SlotTypeOf`: an `alloca`'s allocated type, a global's value type) and falls
  back on the handed layout only for an address that carries none, a GEP into
  an aggregate. Trusting the target expression's layout instead rejects stores
  that are correct.

  The same drift in the other direction is a real corruption, and is now
  refused rather than emitted. An unconstrained literal initialiser used to be
  pinned by the assignment that *declared* the local — to a type read back off
  the local it had just seeded, so `result = 1` stamped `Int32` — which
  consumed the record that let it move later. `result *= base` then took the
  site to `Int8` and left the literal at `Int32`, and the emitter put a
  `store i32` into an `alloca i8`: three bytes past the end of the slot, a
  smashed frame, and a jump to the PIE base at `ret`. Twelve of these existed
  across the stdlib (`Int8/16/64#pow`, `String#hash/trim/upcase/downcase`,
  `Array#push`, `Exception#format_backtrace`). Fixed upstream — the first word
  is no longer spoken, so the last one wins — and `EmitStore` now reports any
  survivor by naming both widths instead of storing it.

## Closures — `ClosureEnvFrame` is already computed

`SynthesizeClosureFrame` hands the emitter `{ Fields[offset], TotalSize,
Alignment, bEscapes }`, and `ClosureEmitter.cpp` allocates that shape and fills
it. Nothing here decides what is captured or where a field lives.

- A `Lambda` and a `Block` differ only in what their body is — an expression
  against a statement list — so both go through one `EmitClosure`. The body
  compiles to a **private** `llvm::Function`: it is reached through its pair,
  never by symbol, so it has no mangling and takes no part in the declare
  sweep's symbol table.
- Parameters are the declared ones **then `ptr %env`**, the trailing position
  `abi.md` fixes for all three targets. The env parameter is present even when
  nothing is captured, so a call site needs no second signature — an empty
  environment is a null pointer, not a missing argument.
- Parameter types come from `UnitTypes::SiteType( BindingSite{ ParamId } )`,
  the only place `| i |` in `arr.each do | i |` has a type at all; the result
  is `Args[0]` of the callable type Sema gave the closure itself.
- `bEscapes == false` (ScopeResolver proved the literal is consumed at its
  call site): the env is an `alloca` in the caller — zero heap, the common
  `do … end` case.
- The body is emitted under a **nested `FunctionFrame`**, with the enclosing
  frame's slots, loop stack and insert point saved and restored around it.
  The frame carries `bClosure`, which two things read (below).

### Captures are addresses, and that is what makes them ordinary

An env field holds the **address** of the captured binding, never a copy of
its value. Two consequences, both load-bearing:

- inside the body a capture is bound by `load ptr` out of the env, which
  yields exactly what `FunctionFrame::Slots` holds for every other binding —
  a place. So `x` reads and writes identically whether it is local or
  captured, and no node kind learns about closures;
- it is the only reading the frame's uniform pointer-sized fields support:
  `SynthesizeClosureFrame` gives every capture the same slot regardless of its
  type, which no by-value copy of an arbitrary aggregate could use. The
  emitter checks each offset still fits the target's pointer size rather than
  assuming the two agree.

### The closure value is a layout, not an emitter-local shape

`{ code, env }` is `abi.md`'s, shared by the three targets — and the stdlib
type claiming `FuncType` / `Lambda` / `Block` declares *no field*, because that
shape is an ABI decision no Volt declaration could express. So it is
materialised in **`BackendCore::InstanceLayouts`**, next to the generic
instantiation it resembles: a nominal claiming one of those three node kinds
resolves to an aggregate of two `Pointer` fields (`Pointer`, not an
`@[Primitive("ptr")]` spelling, so the pair's size follows the target's pointer
size through `LayoutEngine`).

Putting it there rather than in one emitter is what makes a local, a field, a
parameter and an argument holding a callable all agree on the shape — and what
will let the VM and wasm read a closure this backend wrote. The claim is asked
of the store through `@[Literal]`, the same protocol that identifies the type
behind `nil` or a string literal, so no Volt type name enters.

### Invoking one: the resolution says so, the backend does not ask

`f( x )` on a local holding a callable and `block.call( x )` on a `&block`
parameter are the *same* emission. `MemberResolver` resolves both the same
way, and the recognition costs no annotation: the callable type is whichever
type claims the `FuncType` node kind (`IsCallableType`, one
`LookupNodeKind`), and the member invoked is that type's single `abstract`
contract, found by walking its members — so the spelling `call` never enters
C++. The signature is then read off the receiver's own type arguments —
result first, then the parameters — because a callable's arity lives in its
type, not in that contract's declaration.

All of that happens **once**, in Sema, and lands on the resolution as
`CalleeEntry::bIndirect` (next to `bConstructs`). The emitter therefore:

```
Entry.bIndirect  ->  load { code, env } from the receiver, build the
                     FunctionType from Entry.Result / Entry.Params, and
                     call `code` indirectly with `env` appended
otherwise        ->  direct call to the mangled symbol, as before
```

That single bit is the whole backend-facing contract, which is the point: a
second target reads the same bit and re-identifies nothing
(rules/zero-hardcode.md).

This is the one place a `FunctionType` is built from a *type* rather than from
a declaration — a callable has no declaration to build it from. The receiver
expression is the callee's object for a `Member`, and the callee expression
itself for a bare `f( x )`.

A trailing `do … end` fills the callee's `&block` slot, which is **not** a
positional one: `Member::ParamIsBlock` marks it, positional matching skips it
(`MemberResolver`), and `EmitResolvedCall` therefore walks the declared
parameters and the argument list together rather than assuming they are the
same sequence.

### `next` in a block is a `ret`

Inside a closure body with no loop of its own, `next [value]` ends *this
invocation* and hands the value back — so it emits `ret`, not a branch. The
same keyword means "continue" only when there is a loop in this frame to
continue, and `Frame.bClosure` is what tells the two apart. `break` in the same
position is a non-local exit from the method that invoked the block; that needs
an unwinding transport, so it is refused and named as the exception emitter's
(tier 1, below).

## Exceptions — `RaiseExpr` / `BeginExpr`, two tiers

Tier 1 (implemented, `ExceptionEmitter.cpp`): **setjmp/sigsetjmp-free
personality-less unwinding is not attempted** — `raise` never touches the C
stack directly. It stores the exception's address plus its own NominalId into
two thread-local globals (`volt.exc.value` / `volt.exc.tag`,
`NominalId::InvalidValue` meaning "nothing in flight" — the sentinel `TypedId`
already uses, not a second one) and takes the *poisoned path*: a branch to the
innermost `begin` **this function** owns (`FunctionFrame::Rescues`, a stack
exactly like `Loops`), or — with none — an early return carrying no value,
exactly as if the raising call had simply returned.

Every ordinary (non-`@[External]`) call this emitter makes is followed by
`EmitExceptionCheck`: load the tag, and if it is no longer `InvalidValue`,
take the same poisoned path. So a `raise` several calls deep unwinds one `ret`
at a time until some caller's frame is inside a `begin` — simple, portable,
correct, identical semantics to what the VM will do, and it costs the calling
convention nothing: no signature reserves a channel for it (`abi.md`).

`rescue` clause matching compares the raised object's NominalId (loaded from
the tag global) against each clause's own resolved filter —
`Values->SiteType( BindingSite{ ClauseId } )`, which `ExprInferencer` now
records for *every* clause, bound variable or not, since the ancestry test
needs the target regardless of whether the clause also captures it — by
walking `volt.exc.ancestry`, a `[N x i32]` global (`NominalId -> immediate
Super`) built once per module from `TypeStore::Type(Id).Super`. The walk is
the same reflexive, depth-bounded one `IsSubclassOf` performs in Sema
(`TypeResolve.cpp`), done here at runtime because only the *dynamic* side of
the test is unknown at compile time — the emitter never re-resolves a filter
type itself.

`begin`/`rescue`/`ensure` compiles to four blocks: `Dispatch` (clause ladder),
`Ensure`, `Merge`, plus each clause's own body block. The `Rescues` stack
target changes twice, which is what makes `ensure` run on every path without
a raise inside a handler re-entering its own clause ladder:

- while emitting the body, the target is `Dispatch` — the body's own raises
  (or a pending call) get first refusal from this begin's clauses;
- while emitting each clause's body, the target is `Ensure` directly — a
  raise from inside a handler skips straight past clause matching (it must
  not be caught by the same `rescue`) but still runs this `ensure`;
- while emitting `ensure` itself, the stack has been popped back to whatever
  was active outside this `begin` — its own raises propagate normally, they
  do not re-enter this `ensure`.

Falling through every clause with nothing matching leaves the thread-local
state exactly as `Dispatch` found it, so a second check *after* `ensure`
re-propagates it (branch to the outer `Rescues.back()`, or the same poisoned
return) rather than needing separate plumbing for "unhandled". A rescue
variable's slot is bound exactly like a `LocalDecl`'s — `SlotFor` at
`BindingSite{ ClauseId }`, then `EmitStore` (a memcpy for the aggregate
exception object) — and the thread-local state is cleared *before* the
handler body runs, so a call inside it is checked against whatever *it*
raises, never the exception it is currently handling.

Tier 2 (once hot): Itanium zero-cost EH — `invoke` + `landingpad` with a
custom personality; the clause matching logic (the ancestry walk above) is
unchanged, only the transport differs. The choice is an emitter flag, not an
AST concern.

### The last-resort handler — `EmitEntryPoint`/`EmitInitAll`, and why they exist

The poisoned path bottoms out at "return early carrying no value", and for a
body returning an integer that value is `0`. Run it out to the top and a unit
init raising is a silent, successful-looking return — so something has to read
`volt.exc.tag` back after the last one runs, or the obvious way to write a test
asserts nothing at all:

```volt
def assert!( condition : Bool ) -> Void
  if not condition
    raise "..."            # would exit 0 — indistinguishable from success
  end
end
```

That something is **`_V_init_all`** (`EmitInitAll`, `LlvmEmitter.cpp`): a
function the backend hand-rolls, one `call` per unit's `_V_init_<n>` in order,
checking the tag between each and returning early once one is pending rather
than running the rest. It exists to give the *declaration* every stdlib
`@[External( "volt", "_V_init_all" )]` site names (`Prelude.vl`, below) a
body — `DeclareAll` already emits it as an external declaration, the same
shape every other `@[External]` member gets, and this is its one and only
definition. Hand-rolled rather than compiled from a Volt body because there is
no Volt body: no source file's `def` corresponds to "run every unit's
top-level statements in order".

**`EmitEntryPoint` calls `_V_init_all`, then the Volt entry function, and
returns its `i32`.** The entry function's name is a build option
(`EmitOptions::EntryFunction`, default `"__volt_entry"`), not a hardcoded
symbol — the same category `EntrySymbol = "main"` (the *C* entry point) already
is. `DeclareAll` has emitted it as an ordinary free function by the time
`Finalize` reaches this seam; `EmitEntryPoint` looks it up by name through
`TypeStore::LookupFunction` exactly like any other call, and the emitted shell
is nothing but `call i32 @__volt_entry()` followed by `ret`. Reporting an
uncaught exception and choosing the exit status are **that function's own
`begin/rescue`, entirely in Volt** (`source/Lib/Prelude.vl`):

```volt
def __volt_entry -> Int32
  begin
    __volt_run_units()   # = _V_init_all, declared @[External( "volt", "_V_init_all" )]
    0
  rescue e : Exception
    e.report_unhandled()
    1
  end
end
```

No field is read off `Member`, no type name or message byte enters C++: the
`rescue e : Exception` clause is ordinary `BeginExpr` codegen (matched against
`Exception` — the type claiming `@[Literal( RaiseExpr )]`, see
`rules/zero-hardcode.md`), and `report_unhandled` is called on `e` like any
other method, found through ordinary member resolution. A stdlib that wrote a
different prelude, or a different `rescue` body, would change what "uncaught"
means with zero backend changes.

Note the reach: a top-level `libc_exit( main() )` never gets to call `exit`
when a unit init raises, because the post-call `EmitExceptionCheck` inside
`_V_init_all` stops the loop before any later init (or `main`, which is itself
just another top-level statement) runs — so `__volt_entry`'s `rescue` is what
reports it.

The status is one constant (`0`/`1`) rather than the raised type's `NominalId`
— an id is a build-internal number with no meaning outside the process, and 8
bits of exit status is no place to encode one; that choice is Volt's, made in
`__volt_entry`; the C++ side never sees the alternative. Regression sample:
`samples/Codegen/UncaughtRaise.vl`, whose *passing* result is `exit=1`.

`samples/Tests/*.vl` still calls the stdlib's `@[External]` `exit` directly
rather than raising — an explicit code per failing subject is more useful than
a uniform `1` — but it is now a choice rather than a workaround.

### The raised object outlives the frame that raised it

Tier 1 unwinds by *returning*, so an exception whose storage is the raising
function's own `alloca` is dead the moment that function returns — before any
`rescue` more than one frame up copies it out, and long before the last-resort
hook runs with no Volt frame left at all. `EmitRaise` therefore copies the
object into **`volt.exc.storage`** and publishes *that* address in
`volt.exc.value`.

The buffer is one thread-local `[N x i8]`, sized and aligned by `LayoutEngine`
for the widest type that descends from the type claiming
`@[Literal( RaiseExpr )]` — measured over the store, never a fixed constant,
and a raise of something wider is refused by a message naming both numbers. One buffer matches the one-slot tag/value pair:
tier 1 has exactly one exception in flight per thread. Regression sample:
`samples/Codegen/ExceptionMessage.vl`, which reads `e.message.size` two frames
above the raise, where a dangling read returns whatever the stack held.

**Known interaction, resolved.** Constructing the raised object via
`SomeError.new(...)` was filed here as possibly the pre-existing
aggregate-*return* gap surfacing. It was not: `EmitRaise`'s trust in
`EmitExpr( Node.Exception )` handing back a `ptr` is sound, and the crash was
`Exception`'s subclasses being laid out with none of `Exception`'s fields
(`abi.md`, "Inheritance: the base's fields lead"). The aggregate-return gap
itself is untouched and still deferred to the phase-8 verifier work.

## Monomorphisation — `MonoEmitter.cpp`

A generic type's members carry no signature (`FunctionTypeOf` needs concrete
`Params`/`Result`, which only exist once arguments are known) and a generic
method's own body carries no per-expression type either: `TypeChecker`'s
first pass over `Array<T>#push` or `def map<U>` runs with `self` bound to a
*placeholder* instantiation — every one of the type's own generic slots
invalid — so any expression whose type touches `T` or `U` is `MarkDeferred`
rather than guessed at (`rules/core-ast.md`). `DeclareAll`/`DefineAll` both
skip a generic owner and a member with `OwnGenerics > 0` outright for exactly
this reason.

**Why the unit's own `UnitTypes`/`UnitCallees` cannot answer for a generic
body.** Both hold exactly one entry per `ExprId`, because `TypeChecker` runs
once per unit. A generic body's `ExprId`s are shared by *every* instantiation
that ever calls it — `Array<Int32>#push` and `Array<String>#push` are the
same AST nodes — so neither map could hold more than one instantiation's
answer even if the first pass had computed one.

**The request.** `EmitResolvedCall` already builds `FlatArgs` — the pre-order
NominalId flattening of `CalleeEntry::Bindings` (owner's own generics first,
then the method's) — to drive `FunctionFor`/`Instances.OfSignature`. When the
callee's owner is generic or the member has its own generics, that same call
also enqueues a `MonoRequest{ Owner, Name, Args }` on `State::Mono`
(`BackendCore::Monomorphizer`) — `Owner`+`Name` name the member (a type-shaped
key alone is ambiguous: `Array<Int32>` says nothing about *which* member),
`Key()` dedupes globally so `Array<Int32>#push` is only ever drained once no
matter how many call sites reach it, and recursive generics terminate the
same way a recursive layout does.

**The one semantic step, and where it lives.** `Finalize()` calls
`DrainMonomorphizer()`, which pops the queue until empty (draining a body can
itself discover further instantiations — a generic method calling another —
so this runs to a fixpoint, not once). For each request,
`Sema::ReinstantiateBody( Store, Ast, Scopes, Member, Owner, FlatArgs )`
(`Sema/Layout/Instantiate.hpp`) re-runs the type checker's own expression
inferencer over the member's declared body into a **fresh**
`InstantiatedBody{ Values, Callees }`, with `self` and the method's generics
bound to `FlatArgs` — decoded straight into fresh `SemaTypeId`s, since
`NominalId` is the cross-unit, instantiation-independent currency — instead
of the placeholder holes the first pass left. Parameter and result types come
from the already-resolved `Member::Params`/`Result` (`SigTypeId`) through the
public `Sema::Instantiate`, never by re-deriving them from written syntax:
`ResolveTypeExpr`'s `UnitSink::Param` always refuses a generic reference by
design (the concrete-body case it exists for can never write one), so it is
structurally the wrong tool for this. Once `self` and the parameters carry
concrete `SemaTypeId`s, every expression built on them — calls, operators,
field access — resolves through the exact same `LookupMemberOn` /
`UnifySig` machinery a non-generic body already trusts, because it *is* that
machinery, invoked a second time.

This is monomorphisation's only semantic step, and it stays in Sema on
purpose (`rules/core-ast.md`: zero type inference in a backend) — a backend
decides *when* to instantiate, never *how* to type what it finds.
`EmitMonomorphizedBody` then walks the AST exactly as `DefineMember` walks a
concrete body — same parameter-binding loop, same tail rule — except
`FunctionFrame::Values`/`Callees` (new fields, alongside the pre-existing
`Unit`) point at the overlay's `Values`/`Callees` rather than at
`Unit->Values`/`Unit->Callees`; `Unit`/`Ast`/`Scopes` stay the declaring
unit's own, since a generic body's lexical structure does not change under
instantiation, only its types do. Every other emitter function already reads
types and callee resolutions through `Frame.Values`/`Frame.Callees` rather
than through `Frame.Unit` directly, so a concrete body (which sets both to
its own unit's) and a monomorphised one (which sets both to the request's
overlay) are indistinguishable to `ExprEmitter`/`StmtEmitter`/
`ClosureEmitter`/`ExceptionEmitter` — the whole point of the seam.

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
