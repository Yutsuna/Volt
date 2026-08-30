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
The partition policy is Volt's own, `PartitionWithCallees`, and choosing it is
what makes the feature worth having — see below.

The two indirections stack and do not interfere. `@volt.fn.<sym>` is Volt's,
and serves reloading; the stub is ORC's, and serves laziness. They compose
because `CompileOnDemandLayer` puts the target dylib at the front of the
implementation dylib's link order, so the slot's initialiser `&Fn` resolves to
the *stub* rather than to a body — the slot stays a slot, and nothing is
compiled to fill it in.

**The partition policy is the whole design.** LLVM ships two and neither works
here. `compileWholeModule` is eager with extra steps. `compileRequested` — one
function per partition — is the obvious reading of "lazy" and it turns the
feature into a gamble: each partition clones the module skeleton, so compiling
*n* functions one at a time costs more than compiling them together. Measured on
100 defined functions, varying how many the program calls (best of 7,
interleaved, `scripts/jit_lazy_bench.bash crossover`):

| called | eager | `compileRequested` | `PartitionWithCallees` |
|---|---|---|---|
| 0 % | 246 ms | 82 ms (−72 %) | 72 ms (**−70 %**) |
| 25 % | 253 ms | 253 ms (−37 %) | 120 ms (**−52 %**) |
| 50 % | 257 ms | 330 ms (−9 %) | 166 ms (**−35 %**) |
| 100 % | 263 ms | 660 ms (**+68 %**) | 266 ms (**+1 %**) |

`PartitionWithCallees` takes the requested function plus every function this
module defines that its code *names*, transitively. The winning case is
untouched — a module nothing reaches is still never compiled, a function nothing
names is still never compiled — while the losing case flattens to noise, because
a reached module is compiled as one call tree instead of *n* separate partitions.

The line it draws is "named by an instruction". A direct callee qualifies; so
does the body half of a closure pair, whose address is taken by a store in the
block that goes on to call it. A function named only by a **global initialiser**
does not — that is a vtable, and which entry a `dyn Trait` reaches is precisely
what is unknown until it is reached. Those keep their stubs, so dynamic dispatch
pays only for the methods it actually dispatches to.

A never-called function costs 2.19 ms eagerly and 0.16 ms lazily; the residue is
IR emission, which laziness does not touch. On 400 defined and one called, that
is 799 ms against 114 ms (**−85 %**).

What remains is closure-heavy code whose closures are reached through
monomorphisations in `volt.shared`: a partition cannot cross a module, so those
fragment. `samples/Bench/Benchmarks.vl` is the worst case in the tree at +18 %.
`volt run` is lazy by default; `--no-lazy` is the way out.

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

## `-O` — the pass pipeline, and what a transform layer may not do

LLJIT's IR transform layer is the identity by default, so for a long time the
JIT handed instruction selection exactly what the emitter wrote: an alloca per
local, in the entry block, no SSA anywhere. `JitOptions::OptLevel` was declared
and never read — `volt run -O2` accepted the flag and did nothing with it.

`JitCompiler::InstallPipeline` points that layer at PassBuilder, at the level
`Ir::OptimizationLevelOf` reports. That function lives in `BackendLlvmIr` rather
than in either tail because `volt build -O2` and `volt run -O2` are one promise;
the pipelines built around it differ, and legitimately so — see below.

Measured, alternating the two builds inside one loop (never sequentially —
`EPIC_132.md` §10 says why):

| | flag ignored | `-O0` | `-O3` |
| --- | --- | --- | --- |
| run-bound (300k Collatz) | 130 ms | 132 ms | **84 ms** |
| compile-bound (150 fns, all called) | 366 ms | 347 ms | 789 ms |

So `-O0` is a wash on both axes — the O0 pipeline is essentially mem2reg, and
the benefit it would buy is already recovered by the TargetMachine, which
optimises at its own default level whatever `-O` said. It stays on anyway,
because it costs nothing and makes the two tails emit the same IR. `-O1` and up
now buy real speed for real compile time, which is the trade a user asking for
`-O3` is making on purpose.

Two rules the JIT's pipeline follows and the AOT one does not:

**No whole-program prologue.** The AOT tail runs `InternalizePass` then
`GlobalDCE` first, because its module holds `main` and the set of entry points
is closed. A JIT module is a *fragment*: under PerUnit every other unit is in
another module, and under Lazy a partition is a fragment of that. Internalising
on that basis deletes bodies the next module is about to call.

**Nothing promised may be deleted.** ORC reads a module's symbol table when the
module is *added* and promises exactly that set; the transform runs afterwards,
and materialisation fails outright — `Missing definitions in module ...` — if a
promised symbol is gone. Volt's mergeable bodies are `linkonce_odr`, which is
discardable *and* externally reachable: the O0 pipeline's AlwaysInliner inlines
one at every call site in the module and then drops the out-of-line copy. That
is correct for a translation unit and fatal here — it broke all 87 `jit-whole`
tests. `PinPromisedSymbols` raises `linkonce` to `weak` before the pipeline
runs, which is exactly what `IrOptions::bRetainMergeableBodies` does at
emission, for the same reason, one layer earlier. Internal and private
definitions are left alone: ORC never promised those, so deleting one is the
pipeline doing its job.

The back end's own `CodeGenOptLevel` is deliberately *not* set from `-O`, and
that is worth stating because it looks like an omission. There are two levels —
what the IR looks like, and how hard instruction selection then works on it —
and Volt's `-O` has only ever meant the first: the AOT tail calls
`createTargetMachine` with no level at all (`Core/ModuleContext.cpp`), so
`volt build -O0` gets LLVM's `Default` there too. Setting it on the JIT side
alone was tried and reverted: it made `volt run -O0` mean *nothing* optimised,
at 364 ms on the benchmark above. If the back-end level is ever to follow `-O`,
both tails move together.

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

Step 4's store is the only race, and it is a single aligned pointer write
(`std::atomic_ref`, relaxed — the emitted `load ptr @volt.fn.<sym>` is an
ordinary load with no acquire to pair with): a concurrent thread reads either
the old address or the new one, never a mixture. That is the same guarantee
`vm.md` phrased as "the patch window is between two instructions".

### The program runs on its own thread

"A concurrent thread" is not hypothetical. `volt run --watch` starts the program
on a thread of its own (`ProgramThread`, DriverRun.cpp) and begins polling
immediately, without waiting for it to return.

That is what makes a hot reload mean anything for the two programs it is
actually for. With the program on the watch thread, the loop was only reached
*after* the program returned — a server or an event loop never reached it at
all, and the only thing "reload" could mean was "run the whole program again".

Which of the two happens is decided by the program, not by an option:

| at reload time | what happens |
| --- | --- |
| still running | nothing restarts it; the slots it calls through now point at the new bodies, so the change lands at its next call and everything it holds in memory survives |
| already returned | started again, exactly as before #125 — for a script that is the only sensible reading of "reload" |

Liveness is sampled twice, before the emission and after the patch, and either
yes counts. Neither moment alone is right: a program can end during the second a
replacement takes to compile, and a program can end *because of* the patch — a
loop whose condition just became false exits before the store returns.
`jit-reload/Ok/LiveReload` is exactly that program, and its `.after.vl` never
returns when started from scratch, so the test can only pass by patching a
live one.

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

### The vtable is the caller a slot does not reach

A `dyn Trait` call does not load `@volt.fn.<sym>`. It loads the address out of
`@_VTable_<Concrete>_<Trait>`, and that array *holds* the address — nothing
stands between the two to repoint. Patching the slots therefore moved every
direct call and left every dynamic dispatch running the old body.

Three changes fix it, all confined to `ELinkage::Indirect`:

- The array is emitted **writable** (`VTableRegistry`). Constant is what a
  vtable is, and giving that up costs an AOT build its read-only placement and
  its devirtualisation — so the AOT build does not give it up. Indirect
  linkage is the announcement that this program will be reloaded, and it is the
  only thing that flips the bit.
- There is **one array per (concrete, trait) pair in the whole session**. A
  later emission — a replacement unit, a REPL line — asks
  `IrOptions::IsAlreadyDefined` and, when the answer is yes, declares the array
  instead of building a second one. A second copy would be a second answer to
  the same dispatch, and only one of the two would ever be patched.
- `IrGenerator::VTableEntries()` reports every `(array, index, symbol)` the
  emission named, and `JitBackend::PatchVTables` writes the new address into
  each entry whose symbol the reload moved. Same single aligned store as step 4
  above, same guarantee.

Two failures cannot be repaired by writing into the array, so they are refused
alongside the signature and layout checks:

| the new unit | why no store helps |
| --- | --- |
| upcasts to a trait nothing had upcast to | there is no array; the running program was compiled without one |
| moves a method's position in its trait | the slot index is a constant in every dispatch already compiled |

Both are the same one-sided doctrine as the other refusals: sometimes
needlessly strict, never wrong.

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
