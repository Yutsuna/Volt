# Backend spec: LLVM ORC JIT — `volt run` / `volt repl`

The development loop: sub-100ms from `volt run` to output, REPL, and hot
reload. Module `BackendJIT` (`source/Volt/Backend/BackendJIT/`).

This target used to be specified as a bytecode VM (`backend/vm.md`, now
retired). The reasoning for the change is in `PLAN_BACKEND_JIT.md` §1.3; the
short version is that a VM meant writing a second complete code generator,
while a JIT reuses the one that exists and replaces only its tail. The VM
spec's two good ideas — the `FunctionTable` indirection seam and the REPL
session model — survive here, transposed to ORC, and are cited as such below.

**One promise is deliberately broken.** `vm.md` said "zero external
dependencies — the VM ships inside the compiler". A JIT does not: it needs
LLVM. `volt run` and `volt repl` are therefore unavailable in a build
configured `enable_llvm=false` or `enable_jit=false`, and the Driver says so
in a message rather than crashing. That is the price of not writing a second
backend, and it is paid knowingly.

## Where the code lives

`BackendJIT` is the ORC tail only. Everything from core AST to `llvm::Module`
lives in `BackendLlvmIr`, shared with `BackendLLVM` (`llvm.md`). The layering
is what makes `volt run` and `volt build` agree by construction rather than by
discipline:

```
BackendCore ── BackendLlvmIr ─┬─ BackendLLVM   volt build
 (no LLVM)     AST -> Module  └─ BackendJIT    volt run / repl
```

`BackendJIT` never includes a `BackendLLVM` header. That is a build-checkable
invariant, not a convention: `grep -r BackendLLVM source/Volt/Backend/BackendJIT/`
must return nothing.

## The contract

`JitBackend` satisfies `TargetBackend` (`core-interfaces.md`) with the same
four methods every target implements, checked by `static_assert` in its own TU.
It additionally implements `IJitBackend` (`BackendCore/ExecutableBackend.hpp`),
which adds `Run`, `Reload`, `EvalUnit` and `LookupSymbol`.

Those four are virtual where the emitters are not, and that is within the
existing rule: `core-interfaces.md` allows a virtual hop **per unit** and
forbids one **per node**. `Run` is called once per execution. Per-node dispatch
is still `std::visit( Overloaded{ ... } )` in `BackendLlvmIr`, untouched.

`EmitResult::Artifact` is the string `"<jit>"`. The concept demands a path; a
JIT has no file. It is a placeholder, and nothing reads it.

## The stdlib is never compiled here

`volt run` loads the stdlib as a pre-compiled shared library. It does not emit
a single instruction of it.

The artifact already exists and predates this backend: `--stdlib-artifact shared`
produces `~/.cache/volt/stdlib/<FrontendKey>/native/<NativeKey>.so` through
`BuildStdlibArtifact` and `LinkerDriver::LinkSharedLibrary`, keyed by
`ComputeNativeCacheKey` (frontend key × triple × opt level × kind × lto).
`JitStdlibLoader` calls the same `EnsureStdlibArtifact` the AOT path calls,
then attaches the result:

```
DynamicLibrarySearchGenerator::Load( Path, /*GlobalPrefix=*/'\0' )
```

`GlobalPrefix` is nul because ELF does not prefix symbols; Mach-O would want
`'_'`.

Units with `Ordinal < BackendInput::StdlibUnitCount` are declared and never
defined. That invariant — the stdlib always occupies the low ordinals, cache
hit or not — is stated on `BackendInput` and is what makes the skip a
comparison rather than a search. This is stricter than the AOT path, which
still redefines inline-eligible stdlib bodies (`bInlineEligibleOnly`) so the
optimiser can inline them. At O0 the JIT inlines nothing, so it skips
everything.

`_V_init_all` is still built here, over **all** units including the stdlib's,
via `BackendCore::SynthesizeInitAll`. The stdlib's `_V_init_<N>` live in the
`.so` and resolve by name through the generator. Circuit link order is
preserved because `SynthesizeInitAll` reads `BackendInput::Units`, which is
already in that order.

Loading is checked, not assumed: `Attach` looks up `_V_init_0` and
`UnwindTransport::SlotAccessorSymbol` and fails naming the missing symbol.
A hidden symbol in the `.so` is the failure mode worth catching early, and
`WriteSymbolManifest` already writes the `nm --defined-only -g` listing beside
the artifact for exactly this.

## TLS — why JIT'd code does not touch the thread-local globals

Volt's unwind transport is three thread-locals (`UnwindTransport.hpp`):
`volt.exc.value`, `volt.exc.tag`, `volt.brk.flag`. JIT'd code addressing them
directly emits TLS relocations, which JITLink resolves only with
`ELFNixPlatform` plus the `liborc_rt` archive — a versioned bootstrap protocol
between the ORC runtime and the platform, tied to the LLVM release.

So `ExceptionLowering` carries a mode:

| Mode | Who | How the slots are reached |
|---|---|---|
| `ETlsAccess::Direct` | AOT | `llvm::GlobalVariable`, `thread_local` — unchanged |
| `ETlsAccess::Accessor` | JIT | `call ptr @__volt_unwind_slots()`, then `gep` |

The accessor returns the address of the calling thread's slot block. Field
offsets inside that block come from `LayoutEngine` over the block's
`LayoutNode`, never from a hardcoded number — the same ABI authority every
other aggregate goes through (`abi.md`).

The result is that a plain `LLJIT` with a `DynamicLibrarySearchGenerator`
suffices: no `ELFNixPlatform`, no ORC runtime archive, no version coupling.
The cost is one call per slot access, on a path already dominated by a
test-and-branch.

`__volt_unwind_slots` is ordinary native code, where TLS works, and lives in
the stdlib `.so`. Under `--no-stdlib` the compiler process supplies it and
`AddProcessSymbols()` reaches it. The name is declared once, in
`BackendCore/UnwindTransport.hpp`, so no backend invents its own
(`rules/zero-hardcode.md`).

This is not a hole in `rules/backend-machine-only.md`. The rule forbids a
backend calling a raw C function that stands in for a **Volt-level** operation
— allocation, append, a protocol member. The unwind slots are not Volt-level:
they are backend machinery that `UnwindTransport.hpp` already specifies and
already names. Reaching them through an accessor instead of a relocation
changes the addressing mode, not the ownership.

## Module granularity

`BackendLLVM` builds one `llvm::Module` for the whole build. Hot reload and the
REPL need one module per unit, so `BackendLlvmIr` takes a granularity:

- `Whole` — one module. What `volt build` uses, and what `volt run` without
  `--watch` uses: fewer ORC compilation passes, nothing to gain from splitting.
- `PerUnit` — one module per unit. Required by `--watch` and by `repl`.

`PerUnit` changes three things upstream:

1. `FunctionRegistry`'s `symbol -> llvm::Function*` cache is reset per module.
   An `llvm::Function*` belongs to its module; leaking one into the next is a
   crash, not a mis-optimisation.
2. Shared globals — the three unwind slots, `volt.exc.storage`, the preorder
   table, the symbol-name table, `_V_init_all`, the entry function, and the
   `@volt.fn.*` slots — belong to a single **prelude module**.
   `ExceptionLowering` gains `bDefineGlobals`: true for the prelude, false
   elsewhere, where they are external declarations.
3. Module-level globals from a top-level `LocalDecl` are defined in their own
   unit's module and declared external elsewhere.

In practice, with the stdlib `.so` attached, the `.so` defines those shared
globals — it contains `raise` sites of its own. The prelude only declares them.
The question of who defines them arises only under `--no-stdlib`.

Monomorphisation crosses modules unchanged: the queue is build-wide and
deduplicates on `Key()`, so an instantiation is emitted into the module of
whichever unit asked first, and every other module sees a declaration.

**Splitting into modules is not what makes a run lazy.** It is tempting to read
`PerUnit` as "the code a run never reaches costs nothing", and that is wrong.
ORC materialises a whole module the first time anything in it is looked up, and
resolution is transitive: a resolved relocation drags in the callee's entire
module, and `_V_init_all` names every unit that has top-level statements. Under
`PerUnit` a plain run therefore compiles essentially everything it can reach,
which is essentially everything. Deferring a single *function* is what the next
section is for.

## Lazy compilation — one function at a time

`JitOptions::bLazyCompilation` swaps `LLJIT` for `LLLazyJIT`, whose
`CompileOnDemandLayer` replaces each function in an added module with a lazy
re-export stub and compiles the body the first time that stub is **called**.
The partition policy is LLVM's shipped `IRPartitionLayer::compileRequested`:
one function per partition.

The two indirections stack and do not interfere. `@volt.fn.<sym>` is Volt's,
and serves reloading; the stub is ORC's, and serves laziness. They compose
because `CompileOnDemandLayer` puts the target dylib at the front of the
implementation dylib's link order, so the slot's initialiser `&Fn` resolves to
the *stub* rather than to a body — the slot stays a slot, and nothing is
compiled to fill it in.

**It is a bet, not a win.** Measured on 100 defined functions, varying how many
of them the program actually calls (best of 7, interleaved):

| called | eager | lazy | |
|---|---|---|---|
| 0 % | 298 ms | 82 ms | −72 % |
| 25 % | 399 ms | 253 ms | −37 % |
| 50 % | 361 ms | 330 ms | −9 % |
| 100 % | 392 ms | 660 ms | **+68 %** |

The crossover sits near 60–70 % called. Below it the saving is large and grows
without bound — a never-called function costs 2.19 ms eagerly and 0.16 ms
lazily, the residue being IR emission, which laziness does not touch. Above it
the loss is real: `compileRequested` clones the module skeleton once per
partition, so compiling *n* functions one at a time costs more than compiling
them together. `volt run` takes the bet by default; `--no-lazy` declines it.

Three things want the eager answer and get it unconditionally:

- **`:asm`** disassembles the bytes at a symbol's address, and under Lazy that
  address is a stub — a jump, not a function.
- **`:bench`** drops its generation afterwards, and a lazy batch has no
  `ResourceTracker` to drop: `LLLazyJIT::addLazyIRModule` takes a `JITDylib`
  and nothing else.
- **`Reload`** stores a *body's* address into a slot.

The first two are the REPL, which runs eager end to end. The third is why
`OpenReplacement` is eager whatever the session policy — a lazy `volt run` that
reloads gets a lazy boot generation and eager replacements, which is the
intended mix.

A lazy compile that fails lands on `LazyCompileFailed`, reached from JIT-ed
code in the middle of the user's program; it reports and aborts. LLVM's default
for that address is 0, which would be a bare SIGSEGV. `bVerify` is what keeps
it unreachable in practice.

## Hot reload — the indirection table is the seam

This is `vm.md`'s `FunctionTable`, transposed. Under `ELinkage::Indirect`, a
call whose callee is defined in another unit goes through a slot:

```llvm
; direct (default):
%r = call i32 @_V4Math4sqrt(double %x)

; indirect (--watch):
%fp = load ptr, ptr @volt.fn._V4Math4sqrt
%r  = call i32 %fp(double %x)
```

The prelude defines `@volt.fn.<mangled> = global ptr null` per build symbol.
Intra-unit calls stay direct — they are reloaded together with their unit, so
indirection buys nothing there.

Reload of an edited file:

1. Re-lex, parse and sema **that unit only**, against the frozen `TypeStore`.
   Per-file `AstContext` and `StringInterner` make this natural — that is what
   `CompileUnit`'s non-movable, own-everything design was for.
2. Compare the new unit's signature against the recorded one. Refuse loudly on
   either of `vm.md`'s two conditions: a changed function signature, or a
   changed layout for a nominal that may have live instances.
3. Emit a fresh module for the unit into a **new** ORC generation.
4. For each symbol the unit defines, look up the materialised address and store
   it into that symbol's `@volt.fn.*` slot.

Step 4's store is the only race, and it is a single aligned pointer write: a
concurrent thread reads either the old address or the new one, never a mixture.
That is the same guarantee `vm.md` phrased as "the patch window is between two
instructions".

**The old generation is never removed.** `ResourceTracker::remove()` unmaps
executable memory, and a live frame executing there would take a SIGSEGV.
Frames already on the stack finish on the old code — the guarantee `vm.md`
made, kept by retaining rather than by chunks-are-values. The cost is resident
memory: tens of kilobytes per reload, so a thousand reloads stay in the tens of
megabytes. Correctness first; a safe unmap would need to know the live frames,
which this backend cannot.

Refusals are stricter than `vm.md`'s, deliberately. `vm.md` refused a signature
change *for a function with live frames*; the JIT cannot inspect the stack, so
it refuses any signature change. Never wrong, sometimes conservative. The
fallback is a full restart, which this target makes cheap.

## REPL

One `LLJIT`, one growing set of generations. Each line is an incremental
`CompileUnit` on the same Driver — `cli-surface.md`: REPL = incremental units
on the same pipeline — parsed and typed alone against the live `TypeStore`. A
line that fails sema is reported and discarded; the session survives and the
ordinal does not advance.

Linkage is `Direct`: a REPL line is never reloaded. A later line that redefines
a name shadows it, and shadowing was already resolved upstream by the
`ScopeTable`.

**Top-level bindings live outside the JIT.** Storage comes from a chunked host
arena, and each line's module reaches it by absolute symbol:

```
Addr = JitRuntime::AllocateBinding( Mangled, Size, Align )   // sizes from LayoutEngine
JitCompiler::DefineAbsolute( { { Mangled, Addr } } )
```

`vm.md` put these in a synthetic "session globals" aggregate re-offset per line
by `LayoutEngine`. That aggregate grows every line, so its storage must be
reallocated, so its base address moves under already-compiled lines. One
allocation per binding in a chunk arena makes addresses permanently stable and
deletes the problem. Sizes and alignments still come from `LayoutEngine` — the
same ABI authority, applied to a binding instead of an aggregate.

Generations are retained until the session closes. Dropping a line's generation
is safe only if the line defined no function that escaped, and proving that is
work the session does not need to do; retention is bounded by session length.
If it ever matters, `Raii::InferReturnOwnership` and `InferParameterEscape`
already answer the escape question upstream.

An exception left in flight by a line is read through `__volt_unwind_slots`,
reported, and the tag reset to `NoExceptionTag`. The session continues.

## What this backend does not decide

Everything `rules/backend-machine-only.md` and `rules/zero-hardcode.md` say
about the other two targets applies here without exception. The JIT reads
`UnitTypes`, `UnitCallees` and the layouts, and emits. It has exactly two
failure modes — `Unimplemented` and `Error` — and never reports a user-facing
diagnostic about Volt source.

Because it runs the same IR `volt build` runs, it inherits the same gaps: a
program that compiles with `volt build` runs with `volt run`, and one that does
not, does not. `break <value>` stays refused here for the same reason it is
refused there. The JIT is a different tail, not a different language.
