# Volt Interpreter — Architecture Specification

**Component:** `volt run` execution engine (Tier-0 VM + Tier-1 JIT)
**Bootstrap language:** Crystal (self-hosting target later)
**Performance goal:** within ~1.5× of PUC-Lua 5.4 on the interpreter alone; match or beat it once Tier-1 is live.
**Status:** Phase 1 implemented — Tier-0 `case`-dispatch VM runs typed bytecode end-to-end
(`volt run`). Frontend + Semantic/TypedAST + IR + BytecodeCompiler + Tier-0 VM are live for the
v0.1.0 language subset; JIT and RAII are deferred (see AGENTS.md #2b for the exact state).

---

## 1. Goals & Non-Goals

**Goals**
- Instant startup. A 10-line script pays *zero* compilation cost.
- Beat Perl, PHP (no-JIT), Ruby (no-YJIT) on the interpreter alone.
- Reach PUC-Lua territory with the interpreter; pass it with the JIT.
- Exploit static types: the type system already proved what dynamic VMs profile for at runtime.
- One shared frontend with the `volt build` LLVM backend — this engine owns everything *after* typed bytecode.
- **Deterministic memory management via RAII** — no GC pause, no stop-the-world, no runtime overhead.

**Non-Goals**
- No LLVM at run time. LLVM lives only behind `volt build`.
- No speculative/deoptimizing JIT machinery. Types are static, so we never guess.
- No tracing. Tracing VMs exist to *recover* type info we already hold.
- No garbage collector. Lifetimes are tracked statically; cleanup is deterministic.

---

## 2. Layered Overview

```
        ┌──────────────────────────────────────────────────────────┐
        │  FRONTEND  (shared with volt build — NOT owned here)       │
        │  Source → Lexer → Parser → AST → Infer → Sema → TypedAST   │
        └───────────────────────────┬──────────────────────────────┘
                                    │  TypedAST  (fully resolved types)
        ╔═══════════════════════════▼══════════════════════════════╗
        ║  INTERPRETER ENGINE  (this document)                      ║
        ║                                                            ║
        ║   Compiler/   TypedAST → typed bytecode (Chunk)            ║
        ║                  ├ const-fold ├ escape-analysis            ║
        ║                  ├ register-alloc ├ peephole/superinstr    ║
        ║                  └ lifetime-analysis → RAII drop points     ║
        ║                                                            ║
        ║   IR/         Chunk · Opcode · Value · DropMap             ║
        ║                                                            ║
        ║   VM/         TIER-0 register VM (direct-threaded)         ║──┐
        ║                  ├ inline caches ├ interner                ║  │ hot fn
        ║                  └ RAII runtime (ctor/dtor dispatch)       ║  │
        ║                                                            ║  │
        ║   JIT/        TIER-1 Cranelift baseline JIT (lazy)         ║◄─┘
        ║                  ├ tier-up counters ├ code cache           ║
        ║                  ├ bytecode→CLIF translator ├ trampoline   ║
        ║                                                            ║
        ║   Runtime/    object model + native stdlib (Shell, IO…)    ║
        ╚════════════════════════════════════════════════════════════╝
```

The contract between frontend and engine is **TypedAST**.
The contract between Tier-0 and Tier-1 is the **Chunk** (typed bytecode + drop maps).
Nail those two and the rest is swappable.

---

## 3. Source Tree

```
Volt/
├── Source/
│   ├── volt.cr                         # entry point, dispatches CLI subcommands
│   │
│   ├── CLI/
│   │   ├── run.cr                      # `volt run`  → interpreter engine
│   │   └── build.cr                    # `volt build`→ LLVM backend (separate)
│   │
│   ├── Frontend/                       # SHARED — not owned by this spec
│   │   ├── Lexer/
│   │   ├── Parser/                  # Modular recursive-descent parser
│   │   │   ├── Parser.cr           # Main class: state, advance/expect, parse()
│   │   │   ├── Prec.cr             # Precedence levels (None, Assignment, Or, And, ...)
│   │   │   ├── ParseTopLevel.cr    # Program, def/async/class/mixin/component/use
│   │   │   ├── ParseDeclaration.cr # Parameters, type params, return types
│   │   │   ├── ParseType.cr         # SimpleType, GenericType, FuncType, NilableType
│   │   │   ├── ParseExpression.cr  # nud/led precedence climbing
│   │   │   ├── ParseControlFlow.cr # if/unless/while/until/match
│   │   │   ├── ParseBlock.cr       # Block expressions, block params
│   │   │   ├── ParseCall.cr        # dot calls, safe calls, function calls, indexing
│   │   │   └── ParseLiteral.cr     # Grouping, array literals
│   │   ├── AST/
│   │   ├── Types/                      # inference engine → resolved types
│   │   └── Semantic/                   # checks, produces TypedAST
│   │
│   ├── IR/                             # ── THE CONTRACT ──
│   │   ├── opcode.cr                   # opcode enum (typed instructions)
│   │   ├── instruction.cr              # 32-bit packed instruction
│   │   ├── chunk.cr                    # compiled function unit
│   │   ├── value.cr                    # register word + tagged Value for dynamic slots
│   │   └── drop_map.cr                 # per-scope RAII drop points (replaces stackmap.cr)
│   │
│   ├── Compiler/                       # TypedAST → Chunk
│   │   ├── bytecode_compiler.cr        # main lowering pass
│   │   ├── const_fold.cr               # fold literals, drop dead branches
│   │   ├── escape_analysis.cr          # mark stack-allocatable allocations
│   │   ├── lifetime_analysis.cr        # compute object lifetimes → emit DROP opcodes
│   │   ├── register_allocator.cr       # linear-scan allocation
│   │   └── peephole.cr                 # superinstruction fusion
│   │
│   ├── VM/                             # ── TIER 0 ──
│   │   ├── vm.cr                       # direct-threaded dispatch core
│   │   ├── frame.cr                    # call frame / register window
│   │   ├── Dispatch/                   # one file per opcode family
│   │   │   ├── arith.cr
│   │   │   ├── cmp.cr
│   │   │   ├── call.cr
│   │   │   ├── load_store.cr
│   │   │   ├── branch.cr
│   │   │   └── raii.cr                 # INIT / DROP / DROP_SCOPE opcode handlers
│   │   ├── inline_cache.cr             # monomorphic/polymorphic call caches
│   │   └── interner.cr                 # string interning table
│   │
│   ├── JIT/                            # ── TIER 1 ──
│   │   ├── tier_up.cr                  # hotness counters + compile trigger
│   │   ├── translator.cr               # Chunk → Cranelift IR (CLIF)
│   │   ├── Cranelift/                  # FFI bindings to libcranelift
│   │   ├── code_cache.cr               # JIT-compiled function table + invalidation
│   │   └── trampoline.cr               # VM↔native calling-convention bridge
│   │
│   └── Runtime/
│       ├── ObjectModel/                # Class, Method, layout, vtables, ctor/dtor registry
│       └── Shell/                      # System::Shell native impls (File, Directory…)
│
└── Spec/                               # tests, mirrored against Source/ layout
    ├── IR/
    ├── Compiler/
    ├── VM/
    ├── JIT/
    └── Runtime/
```

---

## 3b. Current Implementation Status (v0.1.0)

### Completed Components
- **Source/volt.cr** — Entry point exists and dispatches to CLI
- **CLI/** — Full command structure with Run, Ast, Analyse, Circuit, Format, Version, Help, REPL, Build
- **Frontend/Lexer/** — Complete: Token.cr, Lexer.cr
- **Frontend/Parser/** — Complete modular implementation as documented
- **Frontend/AST/** — ANode, Expr, Decl, Program, TypeNode, Dump
- **Frontend/Types/** — Type inference system
- **Frontend/Semantic/** — Analyser, TypeChecker, Contract, Scope, SignatureTable, Diagnostic
- **Frontend/Diagnostic/** — Full diagnostic system with Catalog, Severity, Label, Suggest, CompilationError
- **IR/** — Complete: Opcode.cr, Instruction.cr, Chunk.cr, Value.cr, DropMap.cr
- **Compiler/** — BytecodeCompiler.cr (complete), Unit.cr (complete), ConstFold.cr, EscapeAnalysis.cr, Peephole.cr (identity stubs)
- **VM/** — Vm.cr (case dispatch), Frame.cr (per-frame registers)
- **VM/Dispatch/** — Arith.cr (complete), Branch.cr, Call.cr, Cmp.cr, LoadStore.cr, Raii.cr (reserved), Native.cr

### Placeholder/Stub Components
- **Runtime/ObjectModel/** — Empty directory, reserved for future
- **Runtime/Shell/** — Empty directory, reserved for future
- **Runtime/Builtins/** — Empty directory, reserved for future
- **JIT/** — Directory exists with empty stubs: TierUp.cr, Translator.cr, CodeCache.cr, Trampoline.cr, Cranelift/
- **Compiler/lifetime_analysis.cr** — Not yet created
- **Compiler/register_allocator.cr** — Not yet created
- **VM/inline_cache.cr** — Not yet created
- **VM/interner.cr** — Not yet created

### Implementation Differences from Architecture
1. **Dispatch Method:** Currently using `case` dispatch in Vm.cr (as planned in architecture #7.1 note), not yet direct-threaded
2. **Register Allocation:** Each frame has its own register array; shared contiguous stack is deferred
3. **Value Representation:** Currently using boxed tagged union; untagged registers + NaN-boxing is deferred
4. **RAII:** DropMap struct exists but INIT/DROP opcodes not yet emitted by compiler
5. **Native Calls:** CALL_NATIVE opcode implemented and working

---

## 4. Memory Model — RAII, not GC

This is the most consequential design choice in the engine, and it aligns Volt with C++ and Rust rather than with Lua, Python, or Ruby.

### 4.1 The thesis

A garbage collector introduces three costs that a shell-first language should never pay:

- **Pause latency** — a GC stop-the-world in the middle of a file traversal loop is visible.
- **Runtime overhead** — write barriers, card tables, and safepoint polls on every object write.
- **Heap pressure coupling** — allocation rate in a tight loop drives GC frequency, creating a non-linear performance cliff.

Volt's static type system already knows object lifetimes. The **compiler can insert drop points deterministically**, the same way C++ destructors work. Objects are destroyed exactly when they go out of scope — not whenever the collector decides to run.

### 4.2 Compiler-emitted RAII opcodes

Lifetime analysis (a new compiler pass) computes the last-use point of every heap-allocated object and emits explicit opcodes into the bytecode:

```
INIT  r1, <ctor: FileInfo>    # allocates + calls constructor — object is live
...
DROP  r1, <dtor: FileInfo>    # calls destructor + frees — deterministic, O(1)
```

For scopes (function exit, loop iteration end, early return), a `DROP_SCOPE` opcode encodes a list of registers to drop in one instruction:

```
DROP_SCOPE  [r1, r3, r7]      # multi-drop on scope exit, ordered dtor call
```

The VM's RAII dispatch handlers (in `VM/Dispatch/raii.cr`) execute the corresponding ctor/dtor directly — no collector involved.

### 4.3 How this maps to Volt syntax

```volt
# The programmer writes this:
Directory.current
  .files(recursive: true)
  .filter { |file| file.extension == "log" }
  .each { |file|
    info = FileInfo.new(file)    # INIT emitted here
    Console.write_line(info.size)
                                  # DROP emitted here — end of block scope
  }

# The compiler emits:
#   INIT  r_info, FileInfo.ctor
#   ...
#   CALL  Console.write_line, r_info.size
#   DROP  r_info, FileInfo.dtor    ← deterministic, not "whenever GC feels like it"
```

`FileInfo` objects are allocated and freed on every loop iteration, predictably. No accumulation, no GC pressure, no pause.

### 4.4 Stack allocation (escape analysis)

Objects that do not escape their scope are **never heap-allocated at all**:

```crystal
# escape_analysis.cr determines that `info` never escapes the each-block.
# The compiler emits stack allocation instead of INIT/DROP.
# Cost: zero. Freed automatically on frame pop.
```

The priority ordering is:
1. **Stack allocation** — if escape analysis proves no escape → free, no opcode
2. **RAII heap** — if the object escapes to a known scope → INIT/DROP pair
3. **Reference counting** — for objects with shared ownership (e.g. passed to async fibers), emit `RC_RETAIN` / `RC_RELEASE` opcodes — still deterministic, still no GC

### 4.5 Reference counting for shared ownership

When the lifetime analysis detects shared ownership (the same object referenced from two live scopes, or passed across an `async` boundary), it switches to reference-count mode for that object:

```
RC_RETAIN  r1          # increment refcount — on share
RC_RELEASE r1          # decrement refcount; if zero → dtor + free
```

RC is only used for genuinely shared objects — the common case (local, unshared) stays RAII. This is the same split Rust makes between owned (`Box`) and shared (`Arc`), just expressed at the opcode level.

### 4.6 The DropMap — replaces the GC stackmap

Since there is no GC, there is no need for safepoint-based heap scanning. Instead each `Chunk` carries a `DropMap`: a list of `(bytecode_offset → [registers + dtors])` entries telling the VM exactly what to drop if an exception unwinds through a given frame.

```crystal
struct DropEntry
  pc_range : Range(UInt32, UInt32)   # bytecodes during which register is live
  register : UInt8
  dtor     : Pointer(Void)           # native dtor function pointer
end

struct DropMap
  entries : Array(DropEntry)
end
```

On normal exit: the compiler emits explicit DROP/RC_RELEASE opcodes — no map consulted.
On exception unwind: the VM walks the DropMap for the current frame and calls outstanding dtors in reverse — deterministic cleanup, identical to C++ stack unwinding.

---

## 5. The Contract: Value Representation

### 5.1 Untagged registers (the fast path)

PUC-Lua tags every value, so every operation reads a tag before acting. Volt knows types statically, so the **opcode carries the type** and registers hold **raw 64-bit words**:

```crystal
# A register is just a 64-bit machine word. No tag.
alias Word = UInt64

# ADD_INT r3, r1, r2  → reads r1,r2 as raw i64, writes raw i64. No tag check.
# ADD_F64 r3, r1, r2  → reads r1,r2 as raw f64 (bitcast), writes raw f64.
# The static type proven by the frontend tells the Compiler which opcode to emit.
```

Monomorphic typed code (the overwhelming majority of Volt) runs **tag-free**. This is the structural edge over Lua.

### 5.2 Tagged `Value` (the dynamic path only)

Genuinely dynamic slots — heterogeneous collections, `Any`, union-typed results — use a NaN-boxed tagged value:

```crystal
# 64-bit NaN-boxed Value, used ONLY where the static type is a union/Any.
struct Value
  @bits : UInt64
  # f64        : stored directly (any non-NaN double)
  # int48      : tag 0x7FFC | payload
  # ptr (heap) : tag 0x7FFD | 48-bit pointer
  # true/false/nil/undef : singleton tags
end
```

**Raw words by default. Tags only where the language is genuinely dynamic.**

---

## 6. Bytecode & Chunk Format

### 6.1 Instruction encoding

Fixed-width 32-bit instructions — keeps the decoder branch-free and cache-friendly.

```
 31      24 23     16 15      8 7       0
┌──────────┬─────────┬─────────┬─────────┐
│  opcode  │    A    │    B    │    C    │   3-register form  (ADD_INT A,B,C)
└──────────┴─────────┴─────────┴─────────┘
┌──────────┬─────────┬───────────────────┐
│  opcode  │    A    │       Bx (16)      │   reg + 16-bit operand (LOAD_CONST)
└──────────┴─────────┴───────────────────┘
```

### 6.2 Chunk — the compiled function unit

```crystal
class Chunk
  name          : Symbol
  arity         : Int32
  num_registers : Int32                  # frame size — known statically
  code          : Slice(Instruction)     # the instruction stream
  constants     : Slice(Value)           # literal pool (interned strings here)
  drop_map      : DropMap                # RAII unwind map (replaces GC stackmap)
  inline_caches : Slice(InlineCache)     # one slot per dynamic call site
  source_map    : Slice(SourceSpan)      # bytecode-pc → source, for errors

  # Tier-1 state
  call_count    : Atomic(UInt32)         # hotness counter
  jit_entry     : Atomic(Pointer(Void))  # null until compiled; set by JIT
end
```

---

## 7. Tier-0 — The Register VM

### 7.1 Dispatch: direct-threaded (computed goto)

The biggest raw interpreter win. Each handler computes the next handler's address and jumps directly — no shared loop head, no mispredicted central branch.

```
Conventional switch:   decode → switch → handler → back to switch   (1 mispredict/op)
Direct-threaded:       handler → decode → jump[next_op]              (0 central branch)
```

> **Implementation note for Crystal:** Crystal doesn't expose `&&label` computed-goto natively. Options in priority order: (a) generate the dispatch table via a macro that builds an array of `Proc`s, (b) drop the inner loop to a small C/asm shim called over FFI, (c) use `case` initially and migrate once the opcode set is stable. Start with (c); prototype (a) or (b) early — it gates a ~25% win.

### 7.2 Frame / register window

```crystal
struct Frame
  chunk     : Chunk
  pc        : Pointer(Instruction)   # current instruction
  base      : Int32                  # offset into the shared register stack
  return_pc : Pointer(Instruction)
end

# One contiguous register stack for the whole VM thread — frames are windows.
# Calls shift `base`. No per-call allocation.
@register_stack : Slice(Word)
@frames         : Array(Frame)
```

### 7.3 Inline caches

Every dynamic call site carries a cache slot. First call resolves and fills it; every call after jumps directly.

```crystal
struct InlineCache
  expected_class : Class?
  resolved       : Method?
  state          : Mono | Poly | Mega
end
```

Static types make most call sites monomorphic — the cache never misses.

### 7.4 Tier-0 win stack (priority order)

| # | Technique | Gain |
|---|---|---|
| 1 | Typed opcodes — no tag checks | structural, always-on |
| 2 | Direct-threaded dispatch | ~20–30% |
| 3 | RAII — no GC pause, no write barriers | latency + throughput |
| 4 | Inline caches | method calls near-free |
| 5 | Stack allocation via escape analysis | zero alloc cost for most loop objects |
| 6 | Superinstructions | fuse hot opcode triples |
| 7 | String interning | `==` → integer compare |
| 8 | Native Shell stdlib | syscall-bound work stays in native code |

---

## 8. Tier-1 — The Baseline JIT

**Engine: Cranelift, not LLVM.** Cranelift compiles 10–100× faster than LLVM. It is built as a fast baseline compiler (Wasmtime, Firefox). Startup stays instant; hot loops hit native speed.

### 8.1 Tier-up trigger

```crystal
THRESHOLD = 1000   # calls + back-edges

def maybe_tier_up(chunk : Chunk)
  if chunk.call_count.add(1) == THRESHOLD
    JIT.enqueue(chunk)   # compiled on a background thread
  end
end
```

Compilation is off-thread. The VM keeps interpreting until `jit_entry` is published. No stop-the-world.

### 8.2 Bytecode → CLIF translation

Static types mean **guard-free, deopt-free** CLIF output:

```
ADD_INT a,b,c      →  v_c = iadd v_a, v_b            (no type guard)
LOAD_FIELD obj, f  →  load.i64 [obj + offset]         (offset known statically)
DROP  r1, dtor     →  call dtor(r1)                   (deterministic, no RC check)
CALL mono          →  call <resolved native entry>    (no inline-cache check)
```

This is the payoff: a dynamic JIT spends most of its code on guards, side-exits, and deopt. Volt emits none of it.

### 8.3 RAII in JIT-compiled code

RAII is, if anything, *easier* in native code than in the VM: the JIT emits direct `call dtor(ptr)` instructions at the drop points the compiler already identified. No runtime overhead beyond the dtor call itself — which C++ pays too.

For RC objects crossing JIT frames, `RC_RETAIN`/`RC_RELEASE` lower to a `lock xadd` on x86-64 — identical to what Swift and ObjARC do in native code.

### 8.4 VM↔JIT boundary

```crystal
entry = chunk.jit_entry.get
if entry
  Trampoline.enter_native(entry, frame_base)   # register stack → native ABI
else
  vm_call(chunk)                               # stay in Tier-0
end
```

Key decisions:
- **Tier-up at call boundaries only** (v1). OSR deferred — adds complexity, gains on single-giant-loop scripts. Phase 6 concern.
- **Shared register-stack ABI.** Native code reads/writes the same register stack — marshaling is near-zero.
- **DropMap spans both tiers** — exception unwind through a native frame consults the same DropMap and calls outstanding dtors in reverse, exactly as C++ stack unwinding does.

---

## 9. Concurrency & `async`

- **One register stack per fiber.** Fibers are stackful green threads (Crystal-native for the bootstrap).
- `await` suspends the current fiber, yields to the scheduler, resumes on completion.
- RAII is fiber-safe by construction: each fiber owns its objects; shared objects use RC with atomic increment/decrement.
- No GC means no cross-fiber safepoint coordination — a significant simplification over GC-based async runtimes.

---

## 10. Build & Dependency Order

```
IR/             ──────────────┐   (no deps — pure data: Opcode, Instruction, Chunk, Value, DropMap)
                              │
Runtime/ObjectModel ──────────┤   (depends on IR for Value; defines ctor/dtor ABI)
                              │
Compiler/       ──────────────┤   (TypedAST → Chunk; depends on IR + Frontend + ObjectModel)
                              │
VM/             ──────────────┤   (executes Chunk; depends on IR + Runtime; no GC dep)
                              │
JIT/            ──────────────┘   (compiles Chunk; depends on IR + VM for the boundary)
```

**Freeze `IR/` early.** Both tiers, RAII unwind, and the JIT translator all depend on its stability.

---

## 11. Implementation Phasing

| Phase | Deliverable | Beats |
|---|---|---|
| 1 | `IR/` + `Compiler/` + Tier-0 `case` dispatch + basic RAII (INIT/DROP) | correctness baseline |
| 2 | Typed opcodes + direct-threaded dispatch | Perl, PHP (no-JIT), Ruby (no-YJIT) |
| 3 | Inline caches + interning + NaN-boxed dynamic Values + escape analysis | matches PUC-Lua 5.4 |
| 4 | RC for shared ownership + DropMap unwind + native Shell stdlib | wins real shell workloads |
| 5 | Cranelift Tier-1, lazy/off-thread, call-boundary tier-up | PHP-JIT, YJIT; ties LuaJIT |
| 6 | Superinstructions, OSR, tuned thresholds | widens margin |

---

## 12. Performance Budget (per-op, Tier-0, rough targets)

| Operation | Target | How |
|---|---|---|
| `ADD_INT` | ~1–2 ns | tag-free, threaded dispatch |
| local load/store | ~1 ns | register window, no boxing |
| monomorphic method call | ~5–8 ns | inline cache hit |
| string `==` (interned) | ~1 ns | integer id compare |
| heap alloc + INIT | ~10–15 ns | malloc + ctor call |
| stack-allocated object | 0 ns | escape analysis — no opcode |
| RAII DROP | ~ctor cost + free | deterministic, no collector |
| RC_RELEASE (last ref) | ~ctor cost + atomic + free | only for shared objects |

---

## 13. Honest Risks

1. **Crystal + computed-goto.** Prototype the threaded dispatch shim in week one — it gates a ~25% win and may need a C/asm helper.
2. **Lifetime analysis is real work.** Correct RAII requires proving lifetimes for every object. Escape analysis is a subset. Budget this properly; mistakes cause use-after-free, not just GC slowness.
3. **RC cycles.** Reference counting cannot collect cycles. Volt needs a rule: either prohibit heap cycles (aggressive) or add a cycle detector for specific types. Define the rule early and encode it in the type system.
4. **LuaJIT is the hard target.** Your structural edge (static types → no guards/deopt) makes a tie feasible on typed numeric loops. Expect that fight to be close.
5. **OSR deferral** hurts single-giant-loop scripts until Phase 6. Acceptable, but documented.
6. **Bootstrap→self-host.** Keep `IR/` free of Crystal-specific assumptions. The RAII model ports cleanly to Volt — it's a first-class language feature anyway.

---

## 14. The One-Sentence Thesis

> Volt is statically typed with known lifetimes, so the interpreter ships **untagged registers + typed opcodes + compiler-emitted RAII** — beating Lua's tagged model *and* eliminating GC pauses entirely — while the JIT ships **guard-free Cranelift code** with direct dtor calls, all with zero startup cost because compilation is lazy, off-thread, and never touches cold code.

---

## Architecutre JSX

```txt
┌─────────────┐     ┌──────────────┐     ┌────────────────┐
│ Fichier .vl │ ──► │ Lexer/Parser │ ──► │ AST Volt+JSX   │
└─────────────┘     └──────────────┘     └────────────────┘
                                                 │
                    ┌────────────────────────────┘
                    ▼
           ┌──────────────┐
           │ Transform JSX │
           │  en appels    │
           │   DOM/Comp    │
           └──────────────┘
                    │
                    ▼
           ┌──────────────┐
           │  Génération  │
           │   Code LLVM  │
           │              │
           └──────────────┘
 ```


| JSX                  | Volt compilé                                           |
| -------------------- | ------------------------------------------------------ |
| `<tag>`              | `Volt::DOM.element("tag", props, children)`            |
| `<Component>`        | `Component.new(props).render`                          |
| `prop={expr}`        | `prop: expr` dans le hash de props                     |
| `on:event={handler}` | Event listener typé dans le hash                       |
| `{expr}`             | Évaluation directe (string interpolation ou composant) |
| Texte brut           | `Volt::DOM.text("...")`                                |
