# Epic #132 — Faster Backend JIT — handoff

State as of 2026-08-28. Branch `Epic/132-backendjit-faster-backend-jit`.

This file exists so the next agent does not re-derive what has already been measured
or re-litigate what has already been decided. Read it before touching `BackendJIT`.

---

## 1. The epic

`gh issue view 132`. Four sub-issues, none closed on GitHub:

| # | Title | State |
|---|---|---|
| **#122** | Perf(Volt): Lazy JIT Tiering | **Steps 1, 2a and 2b done** (2a–2b uncommitted). §3–§5, §8. |
| #121 | Perf(Volt): Heap Snapshot (mmap the stdlib AST + type table) | Not started. **Deprioritise — see §6.** |
| #125 | Feat(BackendJIT): live process hot-reloading in a dedicated thread | **Done, commited.** See §7. |
| #126 | Fix(BackendJIT): update or reject vtable dispatch during hot reload | **Done, commited.** See §7. |

#122 corresponds to milestone **M5** in `.agents/PLAN_BACKEND_JIT.md` §8, whose stated
deliverable is "mesure de démarrage sur un gros circuit". That measurement now exists (§4).

#122 as written bundles two very different jobs. They were split:

- **Step 0 — measure.** Done. §4.
- **Step 1 — laziness** (`LLLazyJIT` / `CompileOnDemandLayer`). Done. §3, §5.
- **Step 2 — tiering** (`ReOptimizeLayer`, re-JIT hot functions at O2). **Done, uncommitted.** §8.

---

## 2. Established facts — do not re-derive these

Each of these cost real time to establish. They were verified against the code and,
where marked, against LLVM 22.1.8's installed headers
(`/nix/store/jlyp0f7r0yaxfygnv0lid2carnsf3v1p-llvm-22.1.8-dev/include/llvm/ExecutionEngine/Orc/`).

**a) `PerUnit` module granularity does *not* make a run lazy.** The comment that used to
claim it in `JitBackend.hpp` was wrong and has been corrected. ORC materialises a whole
*module* the first time anything in it is looked up, and resolution is transitive. Two
mechanisms drag in nearly everything at startup:

1. `_V_init_all` (`EntryPointEmitter.cpp:40`) calls `_V_init_<N>` for **every** unit with
   top-level statements. Each lives in `volt.unit.<N>`, so the whole unit materialises.
2. A resolved relocation pulls in the callee's entire module, transitively.

`ELinkage::Indirect` does not help either: `@volt.fn.X` is defined **in X's own module**
with `Slot->setInitializer( &Fn )` (`FunctionSlots.cpp:107`), so reading the slot is a data
reference into X's module.

**b) The stdlib is already out of scope.** `SkipUnitsBelow` + the `.so`
(`DriverRun.cpp:113`). #122's "50 functions, main calls 2" framing therefore *overstates*
the gain on the common case and *understates* it on `volt.shared`, where every
monomorphisation lives in one module — touching one compiles them all.

**c) Modules emitted under `PerUnit`**: `volt.declarations` (prelude),
`volt.unit.<N>` per unit, `volt.shared` (monomorphisations + `_V_init_all` /
`_V_fini_all` / `__volt_jit_main`). See `IrGenerator.cpp:101,191,230`.

**d) On a 6-line program, 40 function definitions are emitted, of which 2 are the
user's.** Breakdown via `VOLT_JIT_DUMP_IR`: `volt.unit.31` 19, `volt.shared` 8,
`volt.unit.33` 7, `volt.unit.16` 3, `volt.unit.37` 2 (the user's), `volt.unit.18` 1.
Stdlib units reappear above the skip line because of `bDefineCompilerSeamUnits = true`.

**e) Volt slots and ORC stubs compose.** `CompileOnDemandLayer::getPerDylibResources`
puts the **target dylib at the front of the implementation dylib's link order**, so
`&Fn` in the `@volt.fn.X` initialiser resolves to the *stub*, not a body. Volt's
indirection (for reloading) and ORC's (for laziness) stack without interfering.
Confirmed empirically: `jit-reload` passes with a lazy boot generation.

**f) `LLLazyJIT::addLazyIRModule` has no `ResourceTracker` overload** (`LLJIT.h:283` —
only `(JITDylib&, ThreadSafeModule)`). This is *the* architectural friction: a lazy batch
lands on the dylib's default tracker and cannot be removed. `DropGeneration` refuses it.

**g) No unwinder hazard.** Volt's exception transport is setjmp-free and
personality-less (`UnwindTransport.hpp`); there is no `CreateInvoke`/landingpad anywhere
in the backend. The classic LLLazyJIT hazard — an exception crossing a call-through
trampoline — does not exist here.

**h) `JitOptions::OptLevel` is declared and never read** — *was* true; fixed in §8a. The
JIT ran no IR pass at all, at any `-O`. It now runs PassBuilder's pipeline through LLJIT's
IR transform layer. §8b uses it: re-JITting a hot function at O2 re-runs this pipeline at
a higher level, which is what `TierOf` + `RunPipeline` do.

**i) LLVM 22.1.8 has everything needed**, nothing deprecated: `LLLazyJIT` /
`LLLazyJITBuilder` (`LLJIT.h:269,564`), `IRPartitionLayer`, `CompileOnDemandLayer`,
`LazyReexports`, and — for Step 2 — `ReOptimizeLayer.h` with `reoptimizeIfCallFrequent`.

---

## 3. What is implemented (Step 1)

Committed as `f295c9cc`, `f9e55e25`, `549c7495`, `54c8376f`. Ten files:

```
source/Volt/Backend/BackendJIT/Private/JitCompiler.hpp      ECompilePolicy, Init signature, Policy()
source/Volt/Backend/BackendJIT/Private/JitCompiler.cpp      LLLazyJITBuilder, addLazyIRModule, Generations map
source/Volt/Backend/BackendJIT/Private/JitBackend.cpp        passes the policy to Init
source/Volt/Backend/BackendJIT/Public/.../JitBackend.hpp     JitOptions::bLazyCompilation (default false)
source/Volt/Driver/Public/Volt/Driver/Driver.hpp             RunOptions::bLazyCompilation (default TRUE)
source/Volt/Driver/Private/DriverRun.cpp                     wiring
source/Volt/Volt/Public/.../RunCommand.hpp                   bNoLazy
source/Volt/Volt/Private/.../RunCommand.cpp                  --no-lazy flag
.agents/backend/jit.md                                       new "Lazy compilation" section
.agents/rules/cli-surface.md                                 --no-lazy added to the run block
```

Design points worth keeping:

- `ECompilePolicy { Eager, Lazy }`. `Init` takes it as a **request**: if
  `LLLazyJITBuilder::create()` fails (an architecture with no trampolines), it silently
  falls back to a plain `LLJIT`. `Policy()` reports what was actually built.
- `Impl::Trackers` and `Impl::Dylibs` — two parallel maps keyed identically — were merged
  into one `std::map<GenerationId, Generation>`. This is what makes the lazy flag
  obviously correct instead of a third map to keep in sync.
- **`OpenReplacement` is Eager unconditionally**, whatever the session policy. Its two
  callers need what Lazy cannot give: `Reload` needs a *body's* address to store into a
  slot, `:bench` needs `DropGeneration`. A lazy `volt run` that reloads therefore gets a
  lazy boot generation and eager replacements — intended, not an inconsistency.
- `setLazyCompileFailureAddr` points at `LazyCompileFailed`, which reports and aborts.
  LLVM's default is address 0 — a jump to null from inside JIT-ed code.
- Partition policy is Volt's own `PartitionWithCallees`, not LLVM's `compileRequested`.
  See §6 — that choice is what makes lazy a win rather than a gamble.

The REPL is untouched and stays Eager (`Evaluator.cpp` never sets `bLazyCompilation`),
which is what keeps `:asm`, `:bench` and `repl/Generations` correct.

**Verification**: clean `-Werror` build, `meson test -C build` → **586/586 Ok**, no
fixture modified. Differential eager-vs-lazy over the 108 executable samples: 107
identical outputs; the one divergence is `samples/Bench/Benchmarks.vl` printing
non-deterministic tick counters, not a behaviour change.

---

## 4. Measurements

Method: `volt run -v` enables `PhaseTimings` (`RunCommand.cpp:133`). Wall-clock figures
are best-of-N with the two modes **alternated inside the same loop**, so load drift hits
both equally. Harness in §9.

### Step 0 — the cost being attacked (eager, before any change)

`backend.jit.materialize` on a program of N never-called functions plus one that runs:

| N | materialize | total | wasted |
|---:|---:|---:|---:|
| 0 | 28.5 ms | 32.0 ms | — |
| 25 | 76.4 ms | 83.3 ms | 63 % |
| 100 | 215.1 ms | 229.3 ms | 87 % |
| 400 | 773.3 ms | 823.7 ms | 96 % |

Linear: `materialize ≈ 28.5 + 1.87·N` ms. Materialisation is **89–94 % of the run**.

### Step 1 — after laziness

Cost per never-called function: **2.19 ms → 0.16 ms**. The residue is IR emission
(`backend.emit` / `monomorphize` / `verify`), which laziness does not touch. At N=400,
wall clock 952 ms → 134 ms.

### The crossover — this is the number that matters

100 functions defined, varying how many are actually called (best of 7, interleaved).
There is no crossover any more; §6 removed it. Both policies shown because the shape of
the first one is the reason the second exists:

| called | eager | `compileRequested` | `PartitionWithCallees` (current) |
|---|---|---|---|
| 0 % | 246 ms | −72 % | **−70 %** |
| 25 % | 253 ms | −37 % | **−52 %** |
| 50 % | 257 ms | −9 % | **−35 %** |
| 100 % | 263 ms | **+68 %** | **+1 %** |

The dead-weight sweep, one function called and N defined (`sweep`): 65/112/244/799 ms
eager at N = 0/25/100/400, against 61/63/72/114 ms lazy — **−85 %** at N=400.

The one place still worse than eager is `samples/Bench/Benchmarks.vl` at **+18 %**, whose
closures are reached through monomorphisations in `volt.shared`; a partition cannot cross
a module, so those fragment.

Indirect linkage (`--indirect`, what `--watch` turns on) costs ~2–4 % at runtime —
noise. Stacking ORC stubs on Volt slots is affordable.

---

## 5. Decisions taken

**Lazy is on by default** (`RunOptions::bLazyCompilation = true`, `--no-lazy` declines).
Decided by the user, conditional on fixing the regression first — which §6 did.

**The harness lives in `scripts/jit_lazy_bench.bash`**, beside `bench.py` and
`valgrind_check.py`: a manual diagnostic, not a CI test, because the numbers move with
the machine and there is no threshold worth going red on.

---

## 6. The partition policy — the fix that made lazy unconditional

The first cut left `IRPartitionLayer::compileRequested` in place (one function per
partition) and measured **+68 % at 100 % called**: each partition clones the module
skeleton, so compiling *n* functions one at a time costs more than compiling them once.

`PartitionWithCallees` in `JitCompiler.cpp` replaces it — the requested function plus
every function this module defines that its code *names*, transitively. Results
(`scripts/jit_lazy_bench.bash crossover`):

| called | eager | `compileRequested` | `PartitionWithCallees` |
|---|---|---|---|
| 0 % | 246 ms | −72 % | **−70 %** |
| 25 % | 253 ms | −37 % | **−52 %** |
| 50 % | 257 ms | −9 % | **−35 %** |
| 100 % | 263 ms | **+68 %** | **+1 %** |

The line it draws is "named by an **instruction**". A direct callee qualifies, and so
does the body half of a closure pair (its address is taken by a store in the block that
calls it). A function named only by a **global initialiser** does not — that is a vtable,
and which entry a `dyn Trait` reaches is unknown until it is reached, so those keep their
stubs. That distinction is deliberate and is what keeps dynamic dispatch lazy; do not
"simplify" it into walking all references.

An earlier cut restricted this to direct calls only and left `Benchmarks.vl` at +30 %;
widening to instruction operands brought it to +18 %.

**Residual known cost**: closures reached through monomorphisations in `volt.shared`
fragment, because a partition cannot cross a module. `samples/Bench/Benchmarks.vl` is the
worst case in the tree at **+18 %**. Everything else is neutral or better.

Also worth noting for whoever revisits priorities: **#121 is a much smaller lever than
its issue text suggests.** Warm frontend (`parse` + `sema.*` + `seam.*`) is **5.4 ms**;
#121 targets ~2 ms, so it is worth ~3 ms. The 27 ms quoted in #121 is a *cold* figure —
the frontend cache already does that work. On a 100-function file #122 was worth ~187 ms.
Two orders of magnitude apart.

---

## 7. Interaction with #125 and #126

**#125 (run the program in its own thread). Done.**

`volt run --watch` now starts the program on a `ProgramThread` (DriverRun.cpp) and polls
immediately instead of waiting for it to return. Before this, a program that never
returned — a server, an event loop, the only two programs a hot reload is *for* — never
reached the watch loop at all, so "reload" could only ever mean "run the whole thing
again".

Which of the two happens is now decided by the program: still running → nothing restarts
it, the patched slots take effect at its next call; already returned → started again,
exactly as before. **Liveness is sampled twice, before the emission and after the patch,
and either yes counts.** That is not defensive coding, it is the bug I actually hit: the
first cut sampled once, after the patch, and reported "running again" for a program that
had exited *because of* the patch — a loop whose condition had just become false leaves
before the store returns. Do not collapse the two samples back into one.

The `PatchSlots` / `PatchVTables` stores are now `std::atomic_ref` relaxed rather than
bare stores. The machine did the right thing already; the point is that a bare store to
memory another thread reads is a data race by the letter of the standard, and this is
the one window in the whole mechanism.

Test: `jit-reload/Ok/LiveReload`. Its `.after.vl` never returns *when started from
scratch*, so `exit 9` can only be reached by an execution already inside the old loop.
A regression that restarts instead of patching spins forever and the test times out.

Two things checked and left alone:

- `LocalLazyCallThroughManager` performs a **blocking lookup on the calling thread**, and
  there are now two threads in the `ExecutionSession`. ORC supports this
  (`InPlaceTaskDispatcher` runs the task on whichever thread asked), and the full suite is
  green with lazy on by default, which is the combination `--watch` uses. Nothing was
  changed for it. If a deadlock ever shows up here, this is the first place to look.
- `State::Transport` caches the calling thread's unwind slot table. Sound only because
  the one caller of `ExceptionTag` is `EvalUnit` — a REPL, single-threaded. `Run` never
  touches it, which is why the program thread is safe. Whoever gives *EvalUnit* a thread
  has to resolve the accessor per call instead. A comment says so at the declaration.

**#126 (vtables under hot reload). Done — the long-term fix, not the rejection.**

The bug, reproduced before touching anything: `d.area` on a `Dynamic<Shape>` kept
returning the old value after a reload that `PatchSlots` reported as successful. A
`dyn Trait` call reads its callee out of `@_VTable_<Concrete>_<Trait>`, and that array
*holds* the address — there is no `@volt.fn.<sym>` between the two to repoint.

Fixed by making the array patchable instead, all of it gated on `ELinkage::Indirect` so
an AOT build keeps `internal constant` and its devirtualisation (verified on
`samples/Tests/OOP/DynamicDispatch.vl` with `volt build --emit ir`):

1. `VTableRegistry` emits the array writable under indirect linkage, and *declares*
   rather than defines it when `IrOptions::IsAlreadyDefined` says the session already
   has one. That last part is what guarantees a single array per (concrete, trait) pair
   across the whole session — a replacement unit or a REPL line would otherwise build a
   second copy, and only one of the two would ever be patched. It also closes a latent
   REPL bug of the same shape.
2. `IrGenerator::VTableEntries()` reports every `(array, index, symbol)` named.
   `JitBackend::State::VTables` records the build-wide list at `Finalize`,
   `PatchVTables` writes into each entry whose symbol the reload moved. Same single
   aligned store as `PatchSlots`.
3. Two new refusals, in the same one-sided style as the signature and layout ones: a
   unit that upcasts to a trait nothing had upcast to (there is no array), and a unit
   that moves a method's position in its trait (the slot index is a constant in every
   dispatch already compiled).

Tests: `jit-reload/Ok/DynamicCalleeChanged`, `jit-reload/Refused/TraitAdded`,
`jit-reload/Refused/VTableReordered`. The first one fails on `main`.

Interaction with lazy: none that matters. `PatchVTables` looks the array up through
`Compiler.Lookup`, which materialises it exactly as `PatchSlots` already does for a
slot.

Files: `VTableRegistry.{hpp,cpp}`, `EmitterServices.hpp` (`IndirectLinkage` moved out of
`FunctionSlots.cpp` — two files ask now), `IrGenerator.hpp` (`VTableEntry`,
`VTableEntries`, `IsAlreadyDefined`'s contract widened), `IrGenerator.cpp`,
`JitBackend.cpp`, `.agents/backend/jit.md`, `tests/meson.build`, `tests/reload/`.

---

## 8. Step 2 — the pipeline (done) and tiering (done)

### 8a. `-O` now does something. Done, uncommitted.

Fact (h) was the prerequisite and it is discharged. `JitCompiler::InstallPipeline` points
LLJIT's IR transform layer — the identity until now — at PassBuilder, at the level
`Ir::OptimizationLevelOf` reports. That function is new, and it lives in `BackendLlvmIr`
rather than in either tail because `volt build -O2` and `volt run -O2` are one promise;
`BackendLLVM/Private/Target/Optimizer.cpp` now delegates the mapping to it and keeps only
the LTO override, which is the one thing an ahead-of-time build has and a JIT does not.

Measured with the modes **alternated inside one loop**, two binaries side by side:

| | flag ignored | `-O0` | `-O3` |
|---|---|---|---|
| run-bound (300k Collatz) | 130 ms | 132 ms | **84 ms** |
| compile-bound (150 fns, all called) | 366 ms | 347 ms | 789 ms |

`-O0` is a wash on both axes. It stays on because it costs nothing and makes the two
tails emit the same IR. `-O1`+ buys ~35 % faster code for ~2× compile time.

**Do not trust the sequential version of this measurement.** Run one after the other, the
same benchmark reported 167 ms / 137 ms / 84 ms and told a story about mem2reg that
alternating flatly contradicts (§10, first two bullets — I walked into it again).

Two things cost real time and are now written down in `.agents/backend/jit.md`:

- **A transform layer may not delete what ORC promised.** ORC reads the module's symbol
  table when the module is *added*; the transform runs after. Volt's mergeable bodies are
  `linkonce_odr` — discardable *and* externally reachable — so the O0 pipeline's
  AlwaysInliner inlined them and dropped the out-of-line copies, failing materialisation
  with `Missing definitions in module ...`. All 87 `jit-whole` tests went red at once;
  `jit` and `jit-indirect` stayed green because PerUnit gives those bodies external
  linkage. `PinPromisedSymbols` raises `linkonce` to `weak` first, the same fix
  `IrOptions::bRetainMergeableBodies` makes at emission.
- **`CodeGenOptLevel` is deliberately not set from `-O`.** The AOT tail calls
  `createTargetMachine` with no level, so `volt build -O0` gets LLVM's `Default` too.
  Setting it JIT-side only made `volt run -O0` mean nothing-optimised: 364 ms against 130.
  Reverted. If it is ever to follow `-O`, both tails move together.

### 8b. Tiering. Done, uncommitted.

`LLLazyJIT` is replaced by a plain `LLJIT` with a manual lazy stack built on top
(`BuildLazyStack`), because the tiering layer has to go *between* two of the three objects
`LLLazyJIT` would have owned, and `LLLazyJIT` hands out no seam to put it in.

The stack, bottom to top:

```
CompileOnDemandLayer       — splits into partitions, stubs
IRPartitionLayer           — one call tree per partition (PartitionWithCallees)
ReOptimizeLayer            — profiles, re-submits hot partitions with TierFlag
IRTransformLayer           — reads TierFlag → RunOptimizationPipeline(O0 or O2)
ObjectLinkingLayer         — instruction selection, linking
```

**Shared Architecture across Backends (`BackendLlvmIr`).**
Rather than hardcoding optimization logic in `BackendJIT` or duplicating it across `BackendLLVM` and `BackendJIT`, the core optimization and analysis primitives live in `BackendLlvmIr` (`OptimizationLevel.hpp` / `OptimizationLevel.cpp`):
- `Volt::Backend::Ir::RunOptimizationPipeline( Mod, Level, Machine )` — central PassBuilder pipeline runner shared by both AOT (`BackendLLVM/Private/Target/Optimizer.cpp`) and JIT (`BackendJIT/Private/JitCompiler.cpp`).
- `Volt::Backend::Ir::HasLoop( Fn )` and `Volt::Backend::Ir::IsCandidateForOptimization( Fn )` — shared CFG and instruction-complexity analysis helpers.

**Smart Profiler & Tiering Heuristics (`ProfileIfCandidate`).**
Instead of LLVM's default `reoptimizeIfCallFrequent` which blindly instruments every straight-line function (adding counter stores and block splits to 150 trivial functions in `cold.vl`), `JitCompiler` uses a custom profiler function:
1. Functions without loops and under 30 instructions are filtered out and compiled in O0 with **0 profiling overhead**.
2. Candidates with loops/complex bodies use a threshold of 100 calls (`TierThreshold = 100`) before triggering promotion to -O2.

**Placement is the whole decision.** `ReOptimizeLayer` replaces every symbol it takes over
with a jump stub it can later repoint, and a stub is only a thing a *function* can have —
so it declines, silently and by design, any module whose interface holds a data symbol. A
Volt module always does (type tables, globals). Above the partitioning it would therefore
take over the stdlib units and skip the one the user's hot loop is in, which is worse than
not tiering at all. Below it, every partition is pure code — every global it touches is a
declaration resolved back to the module it was split from — so every partition is eligible,
and the unit of promotion becomes the call tree rather than the unit.

`InstallTiering` (`JitCompiler.cpp`) assembles it:

1. `JITLinkRedirectableSymbolManager::Create` — the jump stubs a symbol can be repointed
   through, JITLink's implementation because LLJIT links with JITLink.
2. `ReOptimizeLayer` constructed over `IRTransformLayer`, with `addOrcRTLiteSupport` and
   `registerRuntimeFunctions` on the platform dylib — the in-process stand-in for the ORC
   runtime, defining `__orc_rt_jit_dispatch` as an absolute symbol.
3. `setReoptimizeFunc` sets the tier flag (`volt.jit.tier`) on the module so that the
   transform layer's `TierOf` reads it and runs the pipeline at the promoted level
   instead of at O0.
4. `setAddProfilerFunc` registers `ProfileIfCandidate`.

**The base pipeline runs at O0 when tiering is active.** `Init` passes
`OptimizationLevelOf( bTiered ? 0 : Wanted.OptLevel )` to `InstallPipeline`. A partition
that never crosses the threshold never pays for optimisation. A partition that does is
re-submitted with the tier flag set, re-enters `InstallPipeline`'s transform, hits `TierOf`,
and gets `RunOptimizationPipeline` at the level `-O` named.

**`Tiering()` query** reports whether the tiering layer is in the stack. False for the same
three reasons `Policy()` can differ from what was asked for — no `-O`, no lazy stack, or no
redirectable symbols on this target.

**Tests: `jit-tiered/` suite** (98 tests). Every sample run with `volt run -O2` (lazy +
tiering) must produce the same exit code as without. A promoted function replacing itself
mid-run is the one thing that could quietly change an answer.

**Timing split.** `InstallPipeline`'s transform now times under two names:
`backend.jit.optimize` for the first build, `backend.jit.reoptimize` for a promoted
partition (`TierOf` returns a value). Under Lazy both fire inside the program's own
execution, long after `backend.jit.add` and `backend.jit.materialize` have closed.

Files changed:

```
source/Volt/Backend/BackendLlvmIr/Public/.../OptimizationLevel.hpp   RunOptimizationPipeline, HasLoop, IsCandidateForOptimization
source/Volt/Backend/BackendLlvmIr/Private/OptimizationLevel.cpp     implementations shared by both backends
source/Volt/Backend/BackendLLVM/Private/Target/Optimizer.cpp        delegates default pipeline to BackendLlvmIr
source/Volt/Backend/BackendJIT/Private/JitCompiler.hpp              BuildLazyStack, InstallTiering, Tiering(), SessionOptions doc
source/Volt/Backend/BackendJIT/Private/JitCompiler.cpp              manual lazy stack, ReOptimizeLayer, ProfileIfCandidate,
                                                                      TierFlag/TierOf, PhaseScope timing, data layout validation
tests/meson.build                                                   jit-tiered/ suite (98 tests)
```

---

## 9. Reproduction harness

`scripts/jit_lazy_bench.bash` — a manual diagnostic, not a CI test (the numbers move
with the machine, so there is no threshold worth going red on).

```
scripts/jit_lazy_bench.bash sweep      [reps]   # startup vs functions never called
scripts/jit_lazy_bench.bash crossover  [reps]   # the bet: 100 defined, K called
scripts/jit_lazy_bench.bash samples             # eager and lazy agree everywhere
scripts/jit_lazy_bench.bash files a.vl b.vl     # compare two modes on named programs
```

`VOLT=` overrides the binary path. Two things about its method are load-bearing and
documented in the script's own header: the modes are alternated **inside** one loop
(measuring in two passes produced a confident and completely wrong result), and it uses
wall clock rather than `volt run -v` (§10).

## 10. Traps that cost time

- **`volt run -v`'s `total` lies under lazy.** Compilation migrates into the program's
  execution, which no `PhaseScope` brackets — `backend.jit.materialize` drops to ~0.2 ms
  and the work does not appear anywhere. **Only wall clock is honest for lazy-vs-eager.**
- **The machine is noisy.** Three consecutive runs produced a false "lazy is slower"
  reading that reversed under proper measurement. Always alternate the two modes inside
  one loop and take a min over ≥7 iterations.
- **clangd in this checkout is broken.** It reports `std::string` as `int`, errors inside
  libstdc++ red-black-tree internals, and missing members on `LLLazyJITBuilder` that
  exist. Every one of those is spurious — the real `-Werror` build accepts the code.
  Ignore the IDE diagnostics for `BackendJIT`; trust `ninja -C build`.
- **A green `ninja -C build` does not mean the code compiles.** `build` is Debug (`-Og`);
  `build-release` is `-O3`, and `-Werror` there catches what inlining reveals.
  `PartitionWithCallees` (§6) passed Debug and the full suite, then failed Release with
  `-Wnull-dereference` fired from *inside* LLVM's headers: at -O3 GCC inlines
  `ilist_iterator_w_bits::operator++` far enough to see the intrusive list's sentinel
  `Next == nullptr` and calls walking a basic block a null dereference. It is a false
  positive, the sentinel is what `end()` compares against, and **no spelling of the loop
  avoids it** — block-by-block instead of `llvm::instructions()` moves the message, not
  the cause. Fixed with a `#pragma GCC diagnostic` scoped to that one function;
  `-Wnull-dereference` is deliberately on project-wide (`meson/meson.build`) and is worth
  keeping everywhere else. **Build Release before calling anything in this epic done.**
- **A new `.cpp` needs `meson setup --reconfigure <builddir>`, per build directory.**
  Every module's source list is a `run_command(find ...)` evaluated at *configure* time
  (see any `meson.build`), so ninja never notices a file that did not exist when the
  directory was configured. The symptom is not a missing file — it is `mold: error:
  undefined symbol` at link, which reads like a missing export macro and is not.
- **Never run two builds at once** (`.agents/rules/build-performance.md`, stated in
  capitals). `ninja -C build` for Debug, `ninja -C build-release` for Release,
  `ninja -C build-asan` for ASAN.
- **Format with `ninja -C build format`**, the build's own target — not `clang-format`
  invoked by hand. Once at the end of a phase. `tidy` never (it cannot run: `-freflection`).
- Do not commit or push without being asked.
