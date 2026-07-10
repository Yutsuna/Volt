---
name: crystal-gc-atomic-value-payload
description: "CRITICAL Crystal/Boehm pitfall: pointer-free structs get GC_malloc_atomic (unscanned) containers — IR::Value payload MUST stay Pointer(Void), never UInt64"
metadata: 
  node_type: memory
  type: project
---

Root cause of the worst VM corruption bug in Volt (fixed 2026-07-04): the Phase-4 `IR::Value` rewrite typed `payload : UInt64`, making the struct pointer-free in Crystal's eyes. Crystal then allocates EVERY container of such a type (`Pointer(Value).malloc` = `Vm@stack`, `Array(Value)` = `HeapObject#fields` / chunk constants) with `GC_malloc_atomic` — memory Boehm NEVER scans. Any String/Regex/HeapObject reachable only through a `Value` was invisible to the GC and collected while live.

Symptoms (all context/GC-pressure dependent, hence maddeningly non-deterministic): pcre2 segfaults on regex match, `Thread#main_fiber cannot be nil` crashes, interpolated strings printing empty, object fields resetting to nil, `01.b.Arithmetic.vl`/`01.o.ModuleDatabase.vl` spec failures. Int-only benchmarks (fib/primes) never noticed because they hold no heap refs in Values.

**Fix: `@payload : Pointer(Void)`** (Source/Volt/IR/Value.cr) — keeps the struct pointer-bearing so all containers allocate scanned. Non-pointer payloads ride along as fake pointers (harmless, worst case false retention). Perf unchanged (fib ~1.3s, primes ~0.07s). The original Phase-4 doc argued "conservative scanning ignores static types" — WRONG half: Boehm only scans memory *registered* for scanning, and Crystal decides that per-type.

**Why:** any future Value/repr change must preserve a pointer-typed field, or explicitly use `GC.malloc` (non-atomic) for every Value container.

Related fix same day (Compiler/FunctionEmiter.cr `compile_assign_ident`): first binding `x = y`/`x = self` used to alias x to y's register — rebinding either clobbered the other, and `track_raii_resource` put the shared (param!) register into `raii_regs`, which `execute` nil-inits at frame entry → param wiped before the body read it (linked-list `Sum: 0` bug). Now copies to a fresh register via `place_value`.

Also crucial Crystal enum trap found same session: an enum member named `Nil` does NOT get a usable `.nil?` predicate — `Object#nil?` (always false) shadows it, silently dead-branching `when .nil?` arms. Hit TokenKind::Nil (nil literal never parsed → "VOLT-002") and TypeKind::Nil (Type#to_s). Always spell `== TokenKind::Nil` / use `Type#nil_type?` helper.

See [[volt-perf-milestone]], [[volt-mistral-bug-fixes]].
