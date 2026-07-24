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

## Objects, strings, arrays — stdlib-declared, engine-measured

The compiler has **no object model**. `String` is whatever aggregate the
stdlib declared (`{ data, size }` — StringLiteral's claiming type), `Array`
whatever `source/Lib/Array.vl` declares; a backend only ever sees
`Aggregate{ Fields }` and asks the engine for offsets. Headers, capacity
fields, growth policy — all of it is Volt code in the stdlib, none of it is
C++ (`rules/core-ast.md`: literals materialise from the layout of the type
that claimed the node kind via `@[Literal]`).

Allocation is a call to the stdlib's annotated allocator (`@[External]` /
future `@[Intrinsic]`), so "heap" means the same thing native, VM and wasm —
a symbol the target links, not behaviour the compiler invents.

## Calling convention

| Target | Convention |
|---|---|
| LLVM | C calling convention; `self` first, then declared params, then `ptr %env` for closures. Aggregates by pointer (byval later if profiling asks). |
| VM | stack machine: args pushed left-to-right, `self` first; callee's frame `Base` points at `self`; return value replaces the frame. |
| WASM | wasm params in the same order; aggregates as `i32` addresses into linear memory. |

The *order* (`self`, params, env) is fixed here, once, for all three — it is
what lets a closure emitted by one target's rules be understood by a reader
of any other.

## Closure environments

`SynthesizeClosureFrame` (Sema) already fixed the env aggregate: field
offsets, `TotalSize`, `Alignment`, `bEscapes`. Backends allocate it
(stack when `bEscapes == false`, stdlib heap otherwise) and address captures
by the frame's precomputed offsets. A closure **value** is uniformly the
two-slot aggregate `{ code, env }` — function pointer / `FunctionTable`
index / `call_indirect` table index respectively.

## Exception objects

The exception root is the one stdlib type annotated `@[ExceptionRoot]`
(`TypeStore::GetExceptionRoot`) — the C++ side never spells "Exception".
An in-flight exception is a pointer to an instance of a type whose ancestry
reaches that root; `rescue` clause matching compares dynamic `NominalId`
against the clause's resolved nominal (the ancestry table is emitted per
build as static data). Transport is per-target (`llvm.md` tiers, VM
`Raise`/`EnterRescue`, wasm error slot) but the object and the matching rule
are this section, shared.

## What is deliberately not specified yet

- **GC / ownership**: the stdlib currently manages memory explicitly
  (`Pointer<T>`, `calloc`); a collector is a runtime project that plugs in
  behind the same allocator symbols.
- **Vtables / dynamic dispatch**: every call a backend sees is already
  resolved to a `Member` (static) — there is no `virtual` in the language
  today, so the ABI reserves nothing for it.
- **Integer literal suffixes** (`0_u64` types as the IntLiteral claimant)
  — a Sema gap listed in `rules/core-ast.md`, not an ABI concern, recorded
  here because backends will surface it as "wrong width constant" reports.
