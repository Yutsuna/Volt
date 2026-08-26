# Backend spec: ABI & memory layout — one authority, three encoders

Every target reads sizes, alignments and offsets from **`LayoutEngine`**
(`BackendCore/LayoutEngine.hpp`), never computes its own. That is the whole
ABI story: if two backends disagree about a struct, one of them stopped
calling the engine.

## Layout rules (natural / C-compatible)

Derived from `LayoutNode` alone — no Volt type name enters
(`rules/zero-hardcode.md`):

- `Primitive{ Spelling, Bits }` → size = ⌈Bits/8⌉ bytes, alignment = the
  smallest power of two ≥ size, capped at 8. (`i1` → 1/1, `"i32"` → 4/4,
  `"f64"` → 8/8.)
- `Pointer` → 8/8 on native; the *wasm encoding* of a pointer is `i32`, but
  its in-memory slot stays the engine's numbers on that target's pointer
  size — wasm32 runs the engine with pointer size 4 (a constructor
  parameter when that lands; today the constant is 8).
- `Aggregate{ Fields }` → fields in declaration order, each at its own
  alignment, total padded to the max field alignment. No reordering, ever:
  declaration order is the contract that makes `@[External]` C structs work.

C compatibility is deliberate: an `@[External("libc","calloc")]` boundary
means Volt aggregates must look like C structs under the platform ABI.

### Inheritance: the base's fields lead, flattened

`class Derived < Base` lays out **`Base`'s fields first, spliced flat**, then
its own. Nothing about that is a preference either:

- `super( a, b )` hands `self` straight to `Base#initialize`, which GEPs `@a`
  at the offset *`Base`'s own* layout gives it. A prefix-sharing layout is
  what makes that pointer mean the same thing in both frames — the same
  single-inheritance rule C++ uses, and for the same reason.
- Flat, not nested (`{ base, own… }`), because an inherited `@x` is resolved
  **by name** against the *subclass's* layout (`FieldAddress`), and a nested
  base would hide every inherited name from that lookup.

Two builders produce aggregates and both must obey this: `TypeBinder`'s
`EnsureStructLayout` (`source/Volt/MiddleEnd/Resolver/Private/TypeBinder.cpp`) for a concrete type — reading the parent off the **AST**,
since `NominalType::Super` is only filled in a later phase than the one that
attaches layouts (`Driver.cpp`) — and `BackendCore::InstanceLayouts::Of` for a
generic instantiation, where the parent link is a signature and its arguments
substitute out of `FlatArgs` exactly like a field's. Neither used to look at
the parent at all, which laid every subclass out as a **zero-field aggregate**:
`raise ArgumentError.new( "…" )` had `Exception#initialize` write 40 bytes into
a 0-byte frame slot, and the corrupted frame is what made every `raise`
segfault. Regression sample: `samples/Codegen/Inheritance.vl`.

## Objects, strings, arrays — stdlib-declared, engine-measured

The compiler has **no object model**. `String` is whatever aggregate the
stdlib declared (`{ data, size }` — StringLiteral's claiming type), `Array`
whatever `source/Lib/Array.vl` declares; a backend only ever sees
`Aggregate{ Fields }` and asks the engine for offsets. Headers, capacity
fields, growth policy — all of it is Volt code in the stdlib, none of it is
C++ (`rules/core-ast.md`: literals materialise from the layout of the type
that claimed the node kind via `@[Literal]`).

Allocation is a call to the stdlib's annotated allocator (`@[External]` /
future `@[Intrinsic]`), so "heap" means the same thing AOT, JIT and wasm —
a symbol the target links, not behaviour the compiler invents.

## Calling convention

| Target | Convention |
|---|---|
| LLVM (AOT and JIT) | C calling convention; `self` first, then declared params, then `ptr %env` for closures. Aggregates by pointer (byval later if profiling asks). |
| WASM | wasm params in the same order; aggregates as `i32` addresses into linear memory. |

The *order* (`self`, params, env) is fixed here, once, for all three — it is
what lets a closure emitted by one target's rules be understood by a reader
of any other.

### Aggregates: in by pointer, out by value

The row above says how an aggregate travels *into* a call. Out of one it goes
**by value**, and the caller spills it into a slot of its own frame on arrival.
Two reasons, and neither is a preference:

- The callee's storage does not outlive the callee. Returning its address is
  the one shape that is always wrong; the spill is exactly what makes the
  result the caller's.
- It leaves every signature untouched — no hidden `sret` parameter — so the
  parameter order above stays the whole of the convention, and a `self` slot
  is still argument 0 in every frame.

Everywhere else the rule is unchanged: an aggregate *expression* evaluates to
the address of its storage. The two conversion points are therefore the only
places a value crosses — a `ret` loads the struct, a call's result stores it —
and a target that reads a closure or a struct this one wrote sees a plain C
struct return.

## Closure environments

`SynthesizeClosureFrame` (`MiddleEnd::Resolver`) fixes the env aggregate: field offsets,
`TotalSize`, `Alignment`, `bEscapes`. Unlike the rest of this file, closure
environments are **not a per-target backend concern** any more: `Lambda`/
`Block` are sugar (`rules/core-ast.md`), and `ClosureLifting`
(`source/Volt/MiddleEnd/Lowering/Private/ClosureLifting.cpp`) allocates and addresses the env
itself, upstream, before any backend runs — heap-allocated via an ordinary
`Pointer<UInt8>.malloc( Frame.TotalSize )` call (the stack-when-non-escaping
optimisation is a deferred reclaim, not a blocker; every env is heap today),
with each capture stored and every capturing use rewritten in place into
ordinary `Deref`/`Call( Pointer<T>.from_address, [env-offset expr] )` nodes.
A backend therefore never sees a capture as such — only the `Deref`/`Call`
nodes it already knows how to emit — and needs no closure-specific logic of
its own to reproduce this on a second target. A closure **value** is uniformly
the two-slot aggregate `{ code, env }` — a function pointer natively, a
`call_indirect` table index on wasm.

An env field holds the **address** of the captured binding, not a copy of its
value: the frame gives every capture the same pointer-sized slot whatever its
type, and by-reference is the only reading that supports. It also makes a
capture indistinguishable from a local inside the body — both are a place —
which is why no AST node kind has to know about closures.

The `{ code, env }` shape itself is materialised by
`BackendCore::InstanceLayouts`, not by an emitter: the stdlib type claiming
`FuncType` / `Lambda` / `Block` declares no field, because this is an ABI
decision and no Volt declaration could express it. Both slots are `Pointer`
layouts, so the pair's size follows the target's pointer size through
`LayoutEngine` — four bytes a slot on wasm32, eight native — and one
materialisation serves all three encoders.

## Exception objects

The exception root is the one stdlib type annotated `@[ExceptionRoot]`
(`TypeStore::GetExceptionRoot`) — the C++ side never spells "Exception".
An in-flight exception is a pointer to an instance of a type whose ancestry
reaches that root; `rescue` clause matching compares dynamic `NominalId`
against the clause's resolved nominal (the ancestry table is emitted per
build as static data). Transport is per-target (`llvm.md` tiers, wasm error
slot) but the object and the matching rule are this section, shared.

The JIT reaches the same thread-local slots through an accessor call rather
than a TLS relocation (`jit.md`); that changes the addressing mode, not the
protocol, and the slot names stay the ones `UnwindTransport.hpp` declares.

LLVM Tier 1 (implemented) reserves **no channel in any signature** for this:
the in-flight exception is two thread-local globals (address + NominalId),
and every ordinary call is followed by a check of them — propagation is a
sequence of early returns the caller's own post-call check observes, not an
addition to the calling convention above. See `llvm.md`'s Exceptions section
for the block shape.

## What is deliberately not specified yet

- **GC / ownership**: the stdlib currently manages memory explicitly
  (`Pointer<T>`, `calloc`); a collector is a runtime project that plugs in
  behind the same allocator symbols.
- **Vtables / dynamic dispatch**: every call a backend sees is already
  resolved to a `Member` (static) — there is no `virtual` in the language
  today, so the ABI reserves nothing for it.
- **Integer literal suffixes** (`0_u64` types as the IntLiteral claimant)
  — a MiddleEnd gap listed in `rules/core-ast.md`, not an ABI concern, recorded
  here because backends will surface it as "wrong width constant" reports.
