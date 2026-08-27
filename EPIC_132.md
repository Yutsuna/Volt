# Epic #132 — Faster Backend JIT — handoff

State as of 2026-08-27. Branch `Epic/132-backendjit-faster-backend-jit`.

This file exists so the next agent does not re-derive what has already been measured
or re-litigate what has already been decided. Read it before touching `BackendJIT`.

---

## 1. The epic

`gh issue view 132`. Four sub-issues, none closed:

| # | Title | State |
|---|---|---|
| **#122** | Perf(Volt): Lazy JIT Tiering | **Step 1 done, committed.** See §3–§5. |
| #121 | Perf(Volt): Heap Snapshot (mmap the stdlib AST + type table) | Not started. **Deprioritise — see §6.** |
| #125 | Feat(BackendJIT): live process hot-reloading in a dedicated thread | Not started. Crosses #122, see §7. |
| #126 | Fix(BackendJIT): update or reject vtable dispatch during hot reload | Not started. See §7. |

#122 corresponds to milestone **M5** in `.agents/PLAN_BACKEND_JIT.md` §8, whose stated
deliverable is "mesure de démarrage sur un gros circuit". That measurement now exists (§4).

#122 as written bundles two very different jobs. They were split:

- **Step 0 — measure.** Done. §4.
- **Step 1 — laziness** (`LLLazyJIT` / `CompileOnDemandLayer`). Done. §3, §5.
- **Step 2 — tiering** (`ReOptimizeLayer`, re-JIT hot functions at O2). **Not started.** §8.

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

**h) `JitOptions::OptLevel` is declared and never read** (`JitBackend.hpp`). The JIT runs
at O0 unconditionally; only the stdlib artifact reads that field (`DriverRun.cpp:104`).
Relevant to Step 2: before re-JITting hot functions at O2, the JIT must be able to
compile at O2 at all. That is a small independent task.

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
- Partition policy left at LLVM's default `IRPartitionLayer::compileRequested`
  (one function per partition). See §6 for why that is the thing to revisit first.

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

100 functions defined, varying how many are actually called (best of 7, interleaved):

| called | eager | lazy | |
|---|---|---|---|
| 0 % | 298 ms | 82 ms | −72 % |
| 25 % | 399 ms | 253 ms | −37 % |
| 50 % | 361 ms | 330 ms | −9 % |
| 100 % | 392 ms | 660 ms | **+68 %** |

**Crossover near 60–70 % called.** Above it, `compileRequested` clones the module
skeleton once per partition, so compiling *n* functions one at a time costs more than
compiling them together. Real instance: `samples/Bench/Benchmarks.vl` is **+117 ms
(+33 %)** under lazy.

Indirect linkage (`--indirect`, what `--watch` turns on) costs ~2–4 % at runtime —
noise. Stacking ORC stubs on Volt slots is affordable.

---

## 5. Open decision — the default

`RunOptions::bLazyCompilation` is currently **`true`** (lazy by default, `--no-lazy` to
decline). The reasoning: a program that imports modules and uses a slice of them is the
common case and sits far below the crossover, which is where the large wins are.

**The user was asked and has not yet answered.** The alternative is default off with an
opt-in `--lazy`. The +68 % worst case is bad enough that this should not stay silent.
If you pick up this work, get that answer before committing.

---

## 6. Recommended next step for #122

Rather than accept the bad case, remove it: `IRPartitionLayer::setPartitionFunction`
accepts a custom policy. Partitioning by "the requested function **plus its intra-module
callees**" would compile in useful chunks — far less skeleton cloning, still lazy at
entry granularity — and should flatten the 100 %-called regression. It is bounded work
and the harness in §9 measures it directly.

Also worth noting for whoever revisits priorities: **#121 is a much smaller lever than
its issue text suggests.** Warm frontend (`parse` + `sema.*` + `seam.*`) is **5.4 ms**;
#121 targets ~2 ms, so it is worth ~3 ms. The 27 ms quoted in #121 is a *cold* figure —
the frontend cache already does that work. On a 100-function file #122 was worth ~187 ms.
Two orders of magnitude apart.

---

## 7. Interaction with #125 and #126

**#125 (run the program in its own thread).** `LocalLazyCallThroughManager` performs a
**blocking lookup on the calling thread**. With `CompileThreads = 0` that is synchronous
and safe today, because only one thread is ever inside the `ExecutionSession`. #125 puts
the program on its own thread while the watch loop compiles — two threads in the session
at once. **Do #125 with this in mind, or before further lazy work.**

**#126 (vtables under hot reload).** Under lazy, vtable entries emitted as constant
function pointers receive the *stub* address, which is stable. That neither fixes #126
(a stub from an old generation still reaches the old body) nor worsens it. The
short-term `EReloadStatus::Refused` path proposed in the issue is unaffected.

---

## 8. Step 2 — tiering (not started)

`ReOptimizeLayer.h` exists in LLVM 22.1.8 with `reoptimizeIfCallFrequent` as its default
profiler. Prerequisite: fact (h) above — make `JitOptions::OptLevel` actually do
something first. That is small, independent, and probably worth more in the short term
than tiering itself.

---

## 9. Reproduction harness

**These scripts lived in a session-scratchpad that is now gone. They are inlined here on
purpose.** Where to persist them is still open — `tests/jit/` (M5's stated deliverable is
a startup measurement, so it arguably belongs in the suite) or `scripts/`. The user was
asked and has not answered.

```bash
# gen.sh <n_unused> <out.vl> — N never-called functions plus one that runs.
N=$1; OUT=$2
: > "$OUT"
for i in $(seq 1 "$N"); do
  cat >> "$OUT" <<VOLT
def dead_$i( a : Int32, b : Int32 ) -> Int32
  t = 0
  k = a
  while k < b
    t += ( k * 3 ) % 7
    t -= ( k / 2 ) % 5
    k += 1
  end
  t
end
VOLT
done
cat >> "$OUT" <<'VOLT'
def live( a : Int32 ) -> Int32
  a + 1
end

res = live( 41 )
assert!( res == 42 )
VOLT
```

```bash
# gen2.sh <total> <called> <out.vl> — N defined, K of them called. The crossover sweep.
N=$1; K=$2; OUT=$3
: > "$OUT"
for i in $(seq 1 "$N"); do
  cat >> "$OUT" <<VOLT
def fn_$i( a : Int32, b : Int32 ) -> Int32
  t = 0
  k = a
  while k < b
    t += ( k * 3 ) % 7
    t -= ( k / 2 ) % 5
    k += 1
  end
  t
end
VOLT
done
echo "acc = 0" >> "$OUT"
for i in $(seq 1 "$K"); do echo "acc += fn_$i( 0, 4 )" >> "$OUT"; done
echo 'assert!( acc >= 0 )' >> "$OUT"
```

```bash
# Interleaved wall-clock comparison. Alternating the modes is not optional — see §10.
for f in "$@"; do
  BE=999999; BL=999999
  for r in $(seq 1 9); do
    A=$(date +%s%N); ./build/source/Volt/Volt/volt run --no-lazy -i $f >/dev/null 2>&1; B=$(date +%s%N)
    e=$(( (B-A)/1000000 )); [ $e -lt $BE ] && BE=$e
    A=$(date +%s%N); ./build/source/Volt/Volt/volt run           -i $f >/dev/null 2>&1; B=$(date +%s%N)
    l=$(( (B-A)/1000000 )); [ $l -lt $BL ] && BL=$l
  done
  printf "%-34s eager=%-6s lazy=%-6s delta=%s ms\n" "$(basename $f)" "${BE}ms" "${BL}ms" "$(( BL - BE ))"
done
```

Differential correctness check (eager vs lazy over every executable sample):

```bash
for f in $(find samples/Tests samples/Bench -name "*.vl" | sort); do
  A=$(timeout 60 ./build/source/Volt/Volt/volt run --no-lazy -i "$f" 2>&1); RA=$?
  B=$(timeout 60 ./build/source/Volt/Volt/volt run           -i "$f" 2>&1); RB=$?
  [ "$A" = "$B" ] && [ "$RA" = "$RB" ] || echo "DIVERGENT: $f (rc $RA vs $RB)"
done
```

---

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
- **Never run two builds at once** (`.agents/rules/build-performance.md`, stated in
  capitals). `ninja -C build` for Debug, `ninja -C build-asan` for ASAN.
- **`format` is `clang-format -i -style=file`** over `source/Volt/**`, run once at the end
  of a phase. `tidy` only at the end of the epic.
- Do not commit or push without being asked.
