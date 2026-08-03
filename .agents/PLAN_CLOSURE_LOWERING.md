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

**Phase 0 — Spike (gate, do first).** Prove, on a throwaway no-capture case
(`() => 42`), that a new pre-seam Driver phase (between `ParseOne` and the
`PublishUnitInterface`/`BindUnitTypes` seam) can synthesize a `Method` `Decl`,
append it to `Ast.TopDecls`, and have it resolve, type-check and codegen
exactly like a hand-written `def` — no other phase depends on this working the
way it's currently imagined; if it doesn't, Phase 3 gets redesigned around
whatever `TypeBinder` surgery turns out to be needed instead.

**Phase 1 — `Pointer<T>#to_address`/`to_pointer`/`reinterpret`.** Independent,
can land any time. New op family in the primitive-instruction manifest, three
new `Pointer<T>` stdlib declarations, one new sample proving the round-trip.

**Phase 2 — Function-address-as-value mechanism.** Fixes the diagnosed
`Lambda.vl` bug **standalone**, shippable on its own before any closure-body
lifting exists. New `CalleeEntry` state; `ExprEmitter.cpp`'s `Identifier`/
`Member` visitors read it instead of assuming `bIndirect` implies "call".

**Phase 3 — Full `ClosureLifting` pre-seam phase.** Depends on 0 (design), 1
(reinterpret), 2 (function address). Synthesizes the env `struct` + lifted
`Method` per `Lambda`/`Block`, rewrites the literal into ordinary aggregate
construction. Consider splitting into 3a (no-capture closures) / 3b (capturing
+ escaping) if the deferred-typing wrinkle (a captured binding's real type
isn't known pre-seam, but `reinterpret<U>`'s `U` needs it) turns out to be its
own multi-day problem — likely candidate for the epic's second-biggest risk
after Phase 0. `Lambda`/`Block` move `VOLT_EXPR` → `VOLT_EXPR_SUGAR` in
`Nodes.inl` (24 core / 13 sugar). `ScopeResolver`'s closure-specific capture/
escape logic (`ClosureFrame`/`SynthesizeClosureFrame`) becomes dead code here.

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
