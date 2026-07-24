# Backend spec: bytecode VM — `volt run` / `volt repl`

The development loop: sub-100ms from `volt run` to output, REPL, and hot
reload. Module `BackendVM` (`source/Volt/Backend/BackendVM/`), **zero external
dependencies** — the VM ships inside the compiler.

## Why bytecode (not tree-walking)

The core AST is flat and typed, so a tree-walker would already be decent —
but bytecode wins on the two things this target exists for: hot reload
(patching = swapping a `Chunk` behind an index, impossible to do cleanly
mid-tree) and REPL incrementality (a new line is a new chunk against the same
function table). Emission from the 27 nodes is a single post-order walk;
there is no optimisation pipeline to pay for.

## ISA — `Bytecode.inl` is the manifest

One `VOLT_OP( Name, OperandBytes )` line per opcode generates the enum, the
name LUT and the operand-width table (`Bytecode.hpp`) — disassembler and
dispatch loop derive from the same lines (`rules/meta-first.md`). The set:

| Group | Ops |
|---|---|
| stack | `Nop` `LoadConst(u16)` `Pop` `Dup` |
| locals/env/fields | `LoadLocal(u16)` `StoreLocal` · `LoadUpvalue(u16)` `StoreUpvalue` · `LoadField(u16 byte-offset)` `StoreField` |
| calls | `Call(u16 fn, u8 argc)` · `CallIndirect(u8 argc)` · `CallPrim(u16 op)` · `MakeClosure(u16 fn, u8 caps)` · `Return` |
| control | `Jump(i16)` `JumpIfFalse(i16)` |
| exceptions | `Raise` · `EnterRescue(i16 handler)` `LeaveRescue` |
| `Halt` | end of top-level |

`CallPrim`'s operand indexes a per-build table of `(Spelling, Bits, Op)`
triples the emitter interned — the VM half of the primitive protocol
(`core-interfaces.md`): the *decision* method-vs-instruction was Sema's;
the table only says *which* machine operation.

`LoadField`'s operand is a **byte offset from `LayoutEngine::FieldOffset`**,
not a field index: the VM addresses aggregates exactly like the native
backends (`abi.md`), which keeps `@[External]` data round-trippable.

## Values and frames

- A stack slot is a raw 64-bit `Value` (`VirtualMachine.hpp`). **No NaN-boxing,
  no runtime tags**: the middle-end typed every expression, so what a slot
  holds is statically known at each instruction. Aggregates bigger than a
  slot live in memory and the slot holds their pointer. (Tags can come back
  later for a debugger — as diagnostics, never as semantics.)
- `CallFrame{ Function, Ip, Base }`: locals are `Base + slot`, sized by the
  chunk's `LocalSlots`. One contiguous `std::vector<Value>` stack, frames as
  a parallel vector.
- Dispatch: start with a plain `switch` (portable, fast enough for the dev
  loop); upgrade path is computed-goto / `[[gnu::musttail]]` dispatch behind
  the same `EOpCode` manifest — a loop-local change, invisible outside.

## Emission

Same two-sweep shape as LLVM (`llvm.md`): declare — every reachable `def`
gets a `FunctionTable` index — then define, walking bodies post-order:
expression → push, `Assign` → `StoreLocal`/`StoreField`, `If`/`While`/
`CaseExpr` → `JumpIfFalse` ladders (patched forward like every one-pass
assembler), `Lambda` → emit body as its own chunk + `MakeClosure`,
`begin/rescue` → `EnterRescue` bracket, generic bodies via the shared
`Monomorphizer` queue. The protocol per call site is mechanical: resolved
`CalleeEntry` → `Call`/`CallIndirect`; primitive layout → `CallPrim`.

## Hot reload — the FunctionTable is the seam

Calls never jump to code; they go through `FunctionTable[Index]`
(`Bytecode.hpp` says why). Reload of an edited file is therefore:

1. Re-lex/parse/sema **that unit only** (per-file `AstContext` makes this
   natural — `rules/ast-value.md`), against the frozen `TypeStore`.
2. Re-emit the unit's chunks.
3. `Patch( Index, Fresh )` each function whose name still resolves — under a
   brief pause (the VM is single-threaded per isolate; the patch window is
   between two instructions).
4. Frames already on the stack finish executing their *old* chunk — safe,
   because chunks are values — and every *next* call lands in the new one.

What reload refuses (loudly, like everything else): signature changes to a
function with live frames, and layout changes to types with live instances.
Both are detectable by comparing the old and new `Member`/layout before
patching; the fallback is a full restart, which this target makes cheap
anyway.

## REPL

One `VirtualMachine` + one growing `FunctionTable`; each REPL line compiles
as an incremental unit (`cli-surface.md`: REPL = incremental units on the
same Driver pipeline) whose top-level statements append a fresh entry chunk
executed immediately. Top-level bindings persist by living in a synthetic
"session globals" aggregate, re-offset per line by `LayoutEngine`.
