# Plan — Dehardcoding closures out of the backend (`Lambda`/`Block`)

## Context

`.agents/PLAN_LLVM.md`'s epic is done. The next backend-hardcode target the user
flagged is closures: `source/Volt/Backend/BackendLLVM/Private/ClosureEmitter.cpp`
currently computes/owns the `{code,env}` construction, capture binding, and a
nested-frame closure-body emission — the same shape of problem `ArrayLit`/
`HashLit` used to be before their construction protocol was fully lowered into
ordinary `Call`/`Assign`/`Begin` nodes inside `TypeChecker`
(`rules/backend-machine-only.md`'s "Consequence: ArrayLit/HashLit must be gone
before codegen" section). The user's explicit direction: do the same for
`Lambda`/`Block`, fully, in one epic, rather than patching the immediate bug —
motivated by making the eventual VM/WASM backends need zero closure-specific
code of their own.

**The immediate symptom** that started this investigation:
`samples/Tests/Functional/Lambda.vl` fails with `the callable invoked at
expression N has no receiver expression`. Root cause, confirmed by reading
`ExprEmitter.cpp`'s `Identifier`/`Member` visitors (~line 647, ~1176) and
`ClosureEmitter.cpp`'s `EmitIndirectCall` (~line 336): `EmitIndirectCall` calls
`EmitExpr(Receiver)` to load a closure's `{code,env}` pair, but when `Receiver`
is the very `Identifier` node that carries the `bIndirect` `CalleeEntry`
(`double` in `double(4)`), the `Identifier` visitor's paren-less-bare-call
convenience re-interprets "read the value" as "invoke again", recursing with an
invalid receiver. This is a symptom of the backend making a call-shape decision
it should never have had to make — exactly the class of thing this epic fixes.

## Design (from a Plan-agent investigation, see full findings archived in this
session's transcript — key facts summarized here)

Three genuinely new mechanisms are needed, none of them closure-specific in
themselves:

1. **`Pointer<T>#to_address()`/`UInt64#to_pointer<T>()`** — new machine
   primitives (`ptrtoint`/`inttoptr`, both squarely in the closed
   `u1/i8..i64/u8..u64/f32/f64/pointer/reference/dereference` vocabulary).
   `Pointer<T>#reinterpret<U>()` is then ordinary Volt on top of those two.
2. **A way to reference a resolved callable's address as a value**, distinct
   from calling it — today `Identifier`/`Member` in value position means
   either "read a place" or (ambiguously) "paren-less call"; there is no third
   meaning. This is also the actual fix for the diagnosed bug: `CalleeEntry`
   needs a way to say "this occurrence denotes the callable's address," not
   just `bIndirect` (which describes the *call*, not "a value read of the same
   node that also happens to carry a call resolution").
3. **A pre-seam Driver phase that synthesizes new `Decl`s** (an env `struct`
   + a lifted free function) for every `Lambda`/`Block`. Confirmed via
   `Driver.cpp:323-479` (`CompileRefs`): `TypeStore` is frozen by
   `PublishUnitInterface`/`BindUnitTypes`/`ResolveStructLayouts`/
   `ResolveUnitSignatures`, all of which run **before** `RunSemaOne`
   (the `PassList.inl` manifest, where every existing `Lowering` pass lives).
   No pass in `PassList.inl` can synthesize a decl that ordinary member/
   free-function lookup will ever see — nothing in this codebase has done this
   before. This is the epic's load-bearing, highest-risk mechanism and gets its
   own spike phase before anything else is built on top of it.

Capture/escape detection (`scope.md`) is purely lexical — no type information
needed — which is what makes a *pre-seam* (typeless) lifting phase feasible at
all: every env field can be a uniform `Pointer<UInt8>` (type-erased), sidestepping
the exact ordering problem that forced `ArrayLit`/`HashLit` to lower from
*inside* `TypeChecker` rather than as an ordinary pre-`TypeChecker` pass.

## Phases

**Phase 0 — Spike (gate, do first). DONE, answer is YES.** Proved by injecting
a synthesized `Method` Decl (`def __spike_answer -> UInt64; 42; end`, built by
hand via `Ast.Add(DeclNode{...})`/`Ast.TopDecls.push_back(...)`) into a unit's
`AstContext` right after `ParseOne`, before the `PublishUnitInterface`/
`BindUnitTypes` seam in `Driver::CompileRefs`. A call site in an ordinary
sample (`__spike_answer() == 42`) resolved, type-checked and codegen'd with
*zero* special-casing anywhere downstream — `PublishUnitInterface`,
`BindUnitTypes`, `ResolveUnitSignatures`, `TypeChecker`, `DeclareAll`/
`DefineAll` all walk `TopDecls` purely by content, never by parse-provenance.
Full suite stayed at 231/235 (same 4 pre-existing gaps) with the spike hook
active. The spike code itself was reverted after landing the proof (throwaway,
as planned) — the real mechanism is built properly in Phase 3.

**Phase 1 — DONE, shape changed from the original design.** Landed
`Pointer<T>#to_address() -> UInt64` / `Pointer<T>.from_address(addr) -> Pointer<T>`,
both `abstract` (bodyless — the backend supplies `ptrtoint`/`inttoptr`).
**Discovery**: `EmitBinary`/`EmitUnary`'s "abstract + Primitive/Pointer layout
⇒ backend supplies it" bypass (`Entry->Decl->bAbstract`, checked in those two
functions only) is wired specifically into the *operator* dispatch path — an
ordinary named-method dot-call (`p.to_address()`) goes through `EmitCall` →
`EmitResolvedCall`, which has no such bypass and would try to call a body that
doesn't exist. Added a sibling mechanism, `EmitNamedConversion`
(`ExprEmitter.cpp`, declared in `LlvmState.hpp`), invoked from
`EmitResolvedCall` when `Entry.Decl->bAbstract` and the receiver's layout is
`Pointer`/`Primitive` — same shape as the operator exemption, generalised from
an operator token to a member's own spelling (two names recognized:
`to_address`, `from_address`; anything else abstract-and-unimplemented now
fails loudly instead of silently miscompiling). **Dropped `reinterpret<U>`**
from the original design: Volt has no explicit-generic-argument call syntax on
an *instance* method (`p.reinterpret<U>()` parses `<U>` as a stray positional
argument, not a method generic — confirmed via `volt parse`, a real, separate,
pre-existing parser gap, out of this epic's scope). Reinterpretation is
instead spelled `Pointer<U>.from_address( p.to_address() )`, using only the
already-proven type-generic-instantiation call path (`Pointer<T>.malloc`'s
own shape). Verified round-trip (`malloc` → `to_address` → `from_address` →
dereference) end-to-end; full suite unchanged (231/235, same 4 pre-existing
gaps).

**Phase 2 — DONE, but smaller than planned.** Fixed the diagnosed `Lambda.vl`
bug standalone with **no new `CalleeEntry` state** — the narrower backend-only
fix sufficed: `ClosureEmitter.cpp`'s `EmitIndirectCall` now calls `LoadPlace`
instead of `EmitExpr` when the receiver is the same Identifier/Member node
carrying the call's own `bIndirect` entry (avoids the self-referential
recursion into the paren-less-bare-call heuristic). A second, independent,
pre-existing bug surfaced once the first was fixed: `InstanceLayouts::Of`
(`BackendCore/InstanceLayout.cpp`) checked "already-attached layout wins"
*before* `IsCallable`, so `Proc<R>` (declares no fields) got its vacuous
zero-field TypeBinder layout instead of the `{code,env}` ABI pair — every
closure value was silently zero-sized. Reordered: `IsCallable` now checked
first. Both fixes verified, zero regressions (231/235, same 4 pre-existing
gaps as before: `Composition`, `PointFree`, `Enum`, `UncaughtRaise`).
`Lambda.vl` now passes.

**Phase 3 — `ClosureLifting`, redesigned. Mechanism changed from the plan's
original pre-seam Driver phase — see below.** Depends on 0 (design; the *spike
technique* is reused, not its pre-seam timing), 1 (reinterpret), 2 (function
address).

### Why the original pre-seam design doesn't fit this problem (found before writing code)

Phase 0 proved a `Decl` can be injected before the seam and reach ordinary
resolution. Investigating Phase 3 against that mechanism surfaced two
independent blockers, in this order:

1. **Ordinary construction can't reproduce a closure's type.** `ClosureType`
   (`Sema/.../ClosureInferencer.cpp`) gives `Lambda`/`Block` a multi-arg
   `Proc<Result, Param0, Param1, …>` `SemaTypeId` through a bespoke path
   (`Context.MakeType`) independent of `Proc<R>`'s one declared generic
   parameter. An ordinary `Proc.new(code, env)` call, resolved through normal
   generic instantiation, can never reproduce that arity, so any rewrite going
   through ordinary call resolution breaks unification at the closure's call
   sites. Fix: the rewrite hand-stamps `UnitTypes` on the nodes it creates,
   the same technique `LowerArrayLits`/`LowerHashLits` already use — which
   only a pass running *inside* `TypeChecker`, after the original node's type
   has settled, can do.
2. **But an in-`TypeChecker` sweep can't register the lifted function either —
   not a lookup problem, a data race.** `DeclareAll`/`DefineAll`
   (`BackendLLVM/.../LlvmEmitter.cpp:279`) drive codegen off
   `TypeStore::FreeFunctions()` / `Store.Type(Id).Members`, never off the raw
   AST. That registry is populated once, serially, at the seam
   (`ResolveUnitSignatures`), then read **concurrently, lock-free**, by every
   unit's `TypeChecker` running in `Driver::CompileRefs`'s
   `ForEachUnitParallel(&Driver::RunSemaOne, …)`. Appending a member to
   `Store.FreeFunctions()` from inside that parallel phase is a genuine race
   across units' worker threads. This is why Phase 0's mechanism is
   pre-seam — not for lookup convenience, for **thread safety**: the seam is
   the one serial point the registry may grow at.
3. **The two constraints contradict each other for this specific case.**
   Pre-seam synthesis has no types to give an unannotated closure parameter —
   `arr.each do |i| … end` types `i` purely through `BindClosureParams`'s
   `ExpectedClosure` mechanism, contextual inference a synthesized top-level
   `def` cannot receive before any signature is resolved (`core-ast.md`'s
   "known non-goals": Volt has no bidirectional solver for this). This is not
   only Phase 3b's flagged capture-typing wrinkle — it hits nearly all of 3a
   too, since block-argument closures are usually unannotated.

### The resolution: a per-unit, TypeChecker-populated side table

Register the lifted function **only** into unit-private state, never into the
shared cross-unit `TypeStore` — so there is nothing to race. Concretely:

- A new per-unit table (e.g. `Sema::SynthesizedFunctions`, alongside
  `UnitTypes`/`UnitCallees`/`ScopeTable` in the `PassContext`), populated by
  the `ClosureLifting` sweep while it runs *inside* `TypeChecker`, single
  -threaded, with every capture/param/result type already resolved. Nothing
  outside the unit ever needs to name a lifted closure by symbol — it is only
  ever reached through its own `{code, env}` pair (via the new `FuncAddr`
  node — see Phase 1's design item 2, still unbuilt) — so it needs no
  `TypeStore` membership, no mangled externally-resolvable name, no seam
  participation at all.
- `Backend::UnitView` (`BackendInput.hpp`) gains a pointer/span into this
  table. `DeclareAll`/`DefineAll` gain **one additional, generic walk** over
  each unit's table, reusing the existing `DeclareMember`/`DefineMember`
  codegen paths unchanged — this is the whole backend-side cost, and it is
  identical for every future backend (VM, WASM): "also declare/define this
  unit's synthesized functions," no closure semantics in the walk itself.
- The env buffer is not a new Volt `struct` — captures are `Pointer<UInt8>`
  arithmetic (`to_address`/`from_address`, Phase 1), the same idiom
  `Array.vl`'s own `*(@buffer + index)` already uses. **Scope cut, accepted**:
  since Volt has no source-level fixed-size stack buffer, the env is always
  heap-allocated (`Pointer<UInt8>.malloc`) for now — the non-escaping-closure
  stack optimization `ClosureEmitter.cpp` has today is a regression to
  reclaim in a later phase (its own inert node, or similar), not a blocker
  for this one.
- Captured-variable references inside the lifted body are rewritten in place
  (`ast-rewrite.md`'s copy-out/index-sweep convention) from `Identifier` into
  ordinary `Deref(Call(Pointer<UInt8>.from_address, [env-offset expr]))`
  shaped nodes — genuinely source-level, not backend-nested-frame magic, so
  every backend gets capture-loading for free from its ordinary `Deref`/`Call`
  handling.
- `ScopeResolver` turns out to be exactly the right capture-detection input
  after all — confirmed by reading it end to end: it is **purely lexical**,
  no `TypeStore`/cross-file lookup of any kind (an unresolved identifier is
  never an error there, only a counter), so `RecordCapture`'s output is safe
  to reuse or reproduce without touching the seam. What changed is not the
  capture *detection* mechanism, only **where the resulting rewrite runs**
  (inside `TypeChecker`, not before the seam).

`Lambda`/`Block` still move `VOLT_EXPR` → `VOLT_EXPR_SUGAR` in `Nodes.inl` (24
core / 13 sugar) — `AstInvariant` (order 40) is unaffected by *when* within
`RunSemaOne` the sugar disappears, only that it's gone by the time it runs.
`ScopeResolver`'s existing capture/escape logic (`ClosureFrame`/
`SynthesizeClosureFrame`) stays live for now (still consumed by
`ClosureEmitter.cpp` until Phase 4 deletes it) but is now also the direct
input to the new sweep — no second capture analysis to keep in sync.

Split into 3a (no-capture closures — still needs the side-table + `FuncAddr`
machinery in full, per finding 3 above, so it is *not* the trivial slice the
original plan assumed) / 3b (capturing + escaping, adds the
`Pointer<UInt8>`-arithmetic rewrite and the mandatory-heap-allocation path)
remains the right incremental order — 3a proves the side table and `FuncAddr`
end to end on the simpler rewrite target.

**Phase 3a — DONE.** `Sema::SynthesizedFunctions` (per-unit side table,
`Sema/Layout/SynthesizedFunctions.hpp`) threaded through `PassContext`,
`Driver::CompileUnit`, and `Backend::UnitView`; `LlvmEmitter.cpp` gained
`DeclareSynthesized`/`DefineSynthesized`/`DefineSynthesizedFn`, called from
`DeclareAll`/`DefineAll` exactly as designed — declare-before-any-body-emits,
define after ordinary members, so a `FuncAddr` inside an *ordinary* method
(e.g. `main`) resolves regardless of emission order. `ClosureLifting.cpp`
(new, `Sema/.../TypeChecker/`) runs `LowerClosureLits` inside `TypeChecker`
after the literal lowerings: a no-capture `Lambda`/`Block` (checked via
`ScopeTable::CapturesOf` — empty or absent) lifts its `Params`/`Body` verbatim
into a synthesized top-level `Method` (`Ast.TopDecls`), records it in
`Context.Ctx.Synth`, and rewrites the literal's own slot into `Proc.new(
FuncAddr, nil )` — receiver hand-stamped to the literal's own bespoke
`ClosureType`, then run through ordinary `InferExpr`/call resolution, the same
technique `LowerStringLit` already uses. A closure with a non-empty capture
set is left as `Lambda`/`Block`, on purpose — 3b's scope, not a bug.

**One real bug found and fixed along the way**: `FuncAddr`'s own type (added
in Phase 1) was built from `LookupNodeKind( "CharLiteral" )`, which is `Char`
(`Char.vl`'s own `@[Literal( CharLiteral )]`), not `UInt8` — so a lifted
closure's `code` pointer came back as `Pointer<Char>` while `Proc<R>#code` is
declared `Pointer<UInt8>`, a hard type mismatch at every call site
(`Lambda.vl` regressed from passing to a compile error the moment 3a's sweep
started exercising `FuncAddr` for real). Fixed the same way `LowerStringLit`
avoids spelling "UInt8": read the concrete type off `Proc<R>#code`'s own
already-resolved field signature (`TypeStore::LookupMember( FuncTypeBase,
"code" )` + `Instantiate`), rather than reconstructing a byte-width type from
a node-kind claim nothing declares as `UInt8` specifically. Verified: full
suite back to 232/236 (same 4 pre-existing gaps), `Lambda.vl` passing through
the real `Proc.new`/`FuncAddr`/synthesized-function path end to end.

**`Lambda`/`Block` do NOT move to `VOLT_EXPR_SUGAR` yet.** The corpus has
live capturing closures today (`samples/Sema/ClosureCaptures.vl`,
`Enumerable.vl`'s block-taking methods, `Composition.vl`, `PointFree.vl`,
…) — 3a deliberately skips them, so flipping the sugar flag now would make
`AstInvariant` fail on every one of those files. That flip is Phase 3b's to
make, once the capture/env rewrite lands and nothing depends on `Lambda`/
`Block` surviving `TypeChecker` anymore.

**Phase 4 — Delete `ClosureEmitter.cpp`'s closure-specific machinery.**
Depends on Phase 3 landing completely (no partial state — two systems doing
the same job is worse than one). `EmitIndirectCall` may survive (merged into
`EmitResolvedCall`, since "indirect" is now just "receiver is a `{code,env}`
value," true for any callable-typed value, not closure-specific). `bClosure`
on `FunctionFrame` likely deletable.

**Phase 5 — `break`/`next` re-verification.** No code expected to change (the
non-local-exit transport already looks call-site-generic, not closure-body-
specific — `EmitUnwindCheck` runs after any call) but must be empirically
re-confirmed on a real lifted closure, not assumed.

**Phase 6 — Docs.** `core-ast.md` (24/13 node count), `backend/llvm.md`,
`backend/abi.md`, `middleend/scope.md` updated; this file likely deleted after
landing, per the same convention `PLAN_LITERAL_LOWERING.md` followed.

## Execution order

0 (gate) → 1 ∥ 2 (2 lands standalone, fixes the diagnosed bug) → 3 (needs
0+1+2) → 4 (needs 3 complete) → 5 → 6.

## Checkpoints

A (Phase 0 answered), B (Phases 1+2 shipped, diagnosed bug fixed standalone),
C (Phase 3, no-capture closures only), D (Phase 3 complete), E (Phase 4, backend
actually smaller — track `ClosureEmitter.cpp`'s line count as the concrete
metric).
