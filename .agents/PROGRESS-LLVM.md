# Progress — LLVM Tier 1 finalisation (`.agents/PLAN_LLVM.md`)

Checkpoint updated 2026-07-30 after a second session covering **all of Phase 4**
(4a/4b/4c/4d). The Phase 2/3a/3b work described below was committed in between
(`39ebc97` and its six predecessors); everything from Phase 4 is working tree
only, per project convention (the user commits).

## Suite state

Full `All CTest` (no filter): **320 passing, 14 failed out of 334.**
Session start was 294/334 (40 failed); after the stale goldens were refreshed,
23; after Phase 4, 14.

The 14 break down as:

| Count | Sample(s) | Cause |
|---|---|---|
| 6 | `BreakNext`, `WhileLoop`, `Composition` | Phase 5 — `ArrayLit` construction protocol (**`ArrayLit` itself now shipped**, see below; re-verify these three before assuming green — `BreakNext.vl` still needs Phase 6, others may have separate gaps) |
| 2 | `ForLoop` | Phase 7 — `Hash#each` undeclared (plus the Phase 5 `HashLit` blocker, still open) |
| 2 | `Lambda` | `the callable invoked at expression 25 has no receiver expression` |
| 2 | `PointFree` | `Call`/`Identifier in value position was never given a type` |
| 1 | `Golden.lowered.samples/Sema/CompoundAssignReceiver.vl` | harness gap, see below |
| 1 | `SemaSerializeTest` | pre-existing, see below |

**Two failures that are not compiler bugs and were not introduced here:**

- `Golden.lowered.samples/Sema/CompoundAssignReceiver.vl` — `GoldenTest.cmake:69`
  refuses to write any golden whose output contains `error: `. That file is a
  *negative* Sema fixture whose expected output **is** the diagnostic
  `'+=' needs a target that can be read twice`, so the test is registered but
  its golden can never be generated. Either the skip needs an opt-out for
  negative fixtures, or the test should not be registered for them.
- `SemaSerializeTest` — aborts with `TypedId 1 (arena tag 1) used with arena
  tag 2 of size 2`. The test compares a `NominalId` minted in `Original`
  against `Restored`'s arena (`SemaSerializeTest.cpp:69`), which is exactly the
  Id stability the round-trip is supposed to guarantee, but `VOLT_CHECKED_IDS`
  (`Arena.hpp:115`) treats any Id from another arena instance as foreign.
  Pre-dates both sessions.

## Done this session

**Phase 2 (`CaseExpr::Scrutinee`)** — already committed prior to this session
(`820b35f`, `ddf6bf0`, `3c51111`). Verified correct; fixing it exposed two
latent bugs that were blocking `CaseWhen.vl` from ever reaching them:

- `source/Volt/Backend/BackendLLVM/Private/Instructions.inl` had no LLVM row
  for `TripleEq` (`===`, CaseLowering's synthesized `pattern === target`) in
  any of the three families. Added `VOLT_LLVM_CMP( SInt/UInt/Float, TripleEq,
  ICMP_EQ / FCMP_OEQ )`, same predicate as `EqEq`.
- `source/Lib/Primitives/Symbol.vl` was a plain aggregate struct (`getter id :
  UInt64` + `initialize`) instead of `@[Primitive("u64", 64)]`. The backend's
  `SymbolLiteral` emission (`ExprEmitter.cpp`) requires the claiming type to
  have an integer LLVM layout — same convention as `Bool`/`Int*`. Rewrote to
  match the `Int8`/`Int16` pattern (`self` is the raw value, no stored field).
- `EmitCase` / `EmitBegin` (`ExprEmitter.cpp`, `ExceptionEmitter.cpp`) never
  allocated a result slot for an **aggregate**-typed result (`String`, any
  struct) — silently returned `nullptr` instead. Masked by a second bug: the
  "control fell off the end of a non-void method" epilogue in
  `LlvmEmitter.cpp:421` called `llvm::ConstantInt::get` on the function's
  return type, which for an aggregate return type is an unchecked `cast` in a
  release LLVM build — reads garbage memory as a bit-width (observed as `ret
  i37 0` instead of `{ ptr, i64 }`). Fixed both:
  - `StoreTailValue` now takes a `Sema::LayoutId` and routes an aggregate
    result through `EmitStore`'s memcpy convention instead of a raw
    `CreateStore`; `EmitCase`/`EmitBegin` now always allocate the slot.
  - `LlvmEmitter.cpp:421` now uses `llvm::Constant::getNullValue(...)`,
    matching the already-correct pattern in `ExceptionEmitter.cpp:257` and
    `ClosureEmitter.cpp`'s `CreateUnreachable()`.

**Phase 3a** (branch-scoped implicit locals) and **Phase 3b** (file-scope
globals invisible inside a `def`) — both implemented per the plan's design:

- `ScopeResolver.cpp::Run()` now walks `TopStmts` before `TopDecls`.
- `TypeChecker.cpp::TypeChecker()` — same reordering.
- `ScopeResolver.cpp`'s `Assign` branch declares an implicit `name = expr`
  into `NearestNonBranchScope(Current)` (new helper: walks up while
  `Kind == Branch`, stops at Method/Block/Type/Unit) instead of `Current`
  directly, and calls `SetScopeOfExpr` on the site so the backend's `SlotFor`
  sees the right owning scope.
- `TypeCheckerContext::FindLocal` falls back to `Ctx.Values.SiteType(*Site)`
  when `LocalTypes` (cleared per-method by `EnterMethod`) doesn't have the
  site but `Ctx.Scopes.BindingOf(Use)->Owner` is `EScopeKind::Unit`.
- The plan's noted "asymmetry corollary" (`=` masking a global while `+=`
  doesn't) turned out to need **no separate fix** — once `Found` resolves the
  pre-existing global at scope-resolution time (thanks to the TopStmts-first
  ordering), the existing `Assign` logic already falls through to
  `WalkExpr(Node.Target, Current)` for both operators.

**Fixture bugs** (not compiler bugs, found while chasing the count):
- `samples/Tests/OOP/Classes.vl.expected` was empty — needs `exit=0`. Fixed.
- `samples/Tests/Pointer/PointerNil.vl.expected.vl` was misnamed (should be
  `.../PointerNil.vl.expected`, no trailing `.vl`) — the glob was also picking
  it up as a second, bogus `.vl` sample. Renamed.

## Verified regressions

`samples/Sema/BranchLocals.vl`, `ShadowNestedIf.vl` (explicit `x : T = v`,
untouched by the Phase 3a change) and `RedeclareSameScope.vl` (still reports
its diagnostic) all still behave correctly — checked directly against
`build/build-debug/bin/volt_d`, not yet re-run through `Check.*` CTest.

## Phase 4 — done (second session)

All four sub-phases landed as the plan specified; `IfElsifElse.vl`,
`UnlessElse.vl` and `UntilLoop.vl` all pass Golden + LlvmIr + LlvmRun.

- **4a `then`** — `ParseIf`/`ParseElsif`/`ParseUnless` share one new
  `ParseConditionalTail( If &, CloseHint )` that opens with
  `Accept( TokenKind::KwThen )`. `then` is accepted, never required, and both
  spellings build the *same* tree, so nothing downstream can tell them apart —
  which is what Ruby does and what the user asked for.
- **4b `unless` block** — `ParseUnless` is `ParseIf` with the condition wrapped
  by a new `NegateCond` helper. That helper also replaced `ApplyModifiers`'
  `TokenKind::Bang`, so all four negating forms (`unless`/`until`, block and
  modifier) now spell it `KwNot` — the one `Bool` declares abstract.
- **4c post-test loop** — `Frontend::While` gained `bool bPostTest{}`. Set by
  `ApplyModifiers` when the inner statement is an `ExprStmt` wrapping a
  `BeginExpr` (`WrapsBeginBlock`), read by the loop emitter as a single changed
  edge: entry branches to `Body` instead of `Test`. `LoopFrame` is untouched, so
  `next` still targets the test — the reason for a flag rather than a
  desugaring. The **`while` modifier** (`x += 1 while c`) did not exist at all
  and was added alongside; `KwWhile` joined the `return`/`break`/`next`
  lookahead guards that already excluded `if`/`unless`/`until`.
  The reflective printer picked `bPostTest` up with no printer edit — the dump
  reads `While post_test` on exactly one node in `UntilLoop.vl`.
- **4d `If` → `VOLT_EXPR`** — one manifest line; the struct moved from
  `Stmt.hpp` to `Expr.hpp`. `ParseStatement` lost its `KwIf` arm (both `if` and
  `unless` are now `ParsePrimary` cases and reach statement position through
  `ExprStmt`, like `case`/`begin`). The two consumers migrated exactly as
  predicted: `ScopeResolver`'s arm moved `WalkStmt` → `WalkExpr` unchanged, and
  `StmtEmitter`'s became `ExprEmitter::EmitIf`, converging by slot +
  `StoreTailValue`. A new `IfType` arm in `ExprInferencer` joins the branches'
  `TrailingType`s via `UnifyBranchTypes`, mirroring `CaseType`.
  The tail rule simplified rather than moved: `If` is no longer a second reader
  of `bTail`; the enclosing `ExprStmt` emits the single `ret`.

**Bug found while doing 4d, pre-existing, fixed:** `EmitCase`, `EmitBegin` and
the new `EmitIf` all ended with `CreateLoad( Shape, Slot )`. For an
**aggregate** result that yields an SSA struct, but every consumer of an
aggregate expects an *address* — so `val = case x when 1 then "one" else "many"
end` built a memcpy whose operand was `{ ptr, i64 }` instead of `ptr` and the
module verifier rejected it (`Intrinsic has incorrect argument type!`).
Reproducible on `case` alone, with no `if` involved. All three now go through
one new `State::LoadConverged( Slot, Shape, Layout, Name )` next to
`StoreTailValue`: an aggregate hands back the slot, a scalar is loaded.

**Fixture bugs fixed alongside:**
- `samples/Tests/Conditional/UnlessElse.vl.expected` was empty — no
  `exit=<code>` line, so `LlvmRun` failed even once the sample compiled and ran
  correctly. Same class as `Classes.vl.expected` last session.
- `samples/Tests/Conditional/IfElsifElse.vl:33` asserted
  `val2 == "lesser"` for `val2 = if 10 < 5 then "lesser" else "greater" end`.
  The condition is false, so the value is `"greater"`. Verified the four
  `then` forms independently before touching the assertion.

## Not started — remaining phases

- **Phase 5**: **redesigned, then `ArrayLit` shipped — see
  `.agents/PLAN_LITERAL_LOWERING.md`'s "Status" section for the current
  state, which is the source of truth from here on rather than this entry.**
  The original §5a-§5f design (backend-side `EmitArrayLit`/`EmitHashLit`,
  `@[LiteralAppend]`, widened `UnitCallees`) was superseded before any of it
  was implemented: `@[LiteralAppend]` would have been a 4th annotation beyond
  the closed list (`@[Primitive]`/`@[External]`/`@[Literal]`), and more
  fundamentally the backend may not dispatch on `ArrayLit`/`HashLit` *at all*,
  even read-only (`rules/backend-machine-only.md`). A first attempt at a
  backend-side `EmitArrayLit` built the aggregate by hand via a raw C-ABI
  `malloc` + guessing which struct field was the buffer by LLVM type shape —
  rejected: it hardcodes Array's *structure* instead of its *name*, and
  doesn't generalise to `Hash`'s own shape.
  - **`ArrayLit`: done, a later session.** `Array` gained `<<` (real Volt
    body, resolved through the ordinary operator-resolution path — no
    hardcoded member name, no annotation), and the literal is rewritten into
    ordinary `Begin`/`Assign`/`Binary`/`Call` nodes by a post-walk sweep
    inside `TypeChecker` (not inline — an inline version shipped first and
    broke on `sum_all( [4, 5, 6] )`-shaped code; see the plan doc for why).
    `ExprEmitter.cpp`'s `ArrayLit` dispatch arm is gone; `AstInvariant`
    confirms none survive. This clears the `ArrayLit` part of what blocked
    `WhileLoop.vl`/`BreakNext.vl`/`Composition.vl`/`ForLoop.vl` below — it
    does **not** mean those four are green now, only that Phase 5's
    `ArrayLit` half of their blockage is gone; re-verify each against its
    other listed gaps (Phase 6 for `BreakNext.vl`, Phase 7 for `ForLoop.vl`,
    and whatever `WhileLoop.vl`/`Composition.vl` still need per the table
    above) before crossing them off.
  - **`HashLit`: not started.** `FailAggregateLiteral` still guards it alone.
  - Two `EmitResolvedCall`/`LlvmState.hpp` edits from an earlier abandoned
    attempt in this session (a `ReceiverValue` parameter for §5a) were made
    and then reverted before commit — no trace should remain in the working
    tree; if `EmitResolvedCall`'s signature shows a 5th parameter, that's a
    leftover to remove.
- **Phase 6**: non-local `break` transport (shared unwind check with the
  exception machinery, `volt.brk.flag`, consumption at `EmitResolvedCall`).
- **Phase 7**: `Hash#each` (~10 lines of Volt) and `Range<T>`/`..` operator
  (exclude `..`/`...` from the primitive-op exemption, declare on
  `Comparable`, `Range.vl` stdlib type). Blocks: `ForLoop.vl` (`Hash has no
  member 'each'`), and indirectly anything using `1..5`.
- `Functional/Lambda.vl` currently fails with a **new, not-yet-diagnosed**
  error: `the callable invoked at expression 25 has no receiver expression` —
  worth checking whether Phase 0's rewrite (point-free → typed lambda) is
  still intact in the sample, or this is a fresh regression/gap in `@[Apply]`
  resolution. Not investigated this session.
- `Functional/PointFree.vl` fails with `Call`/`Identifier in value position
  was never given a type` on the `>>` composition chain — likely the same
  bidirectional-inference non-goal noted in `rules/core-ast.md`, needs
  confirming the sample matches the Phase 0 rewrite before assuming it's a new
  bug.

## Not yet done before closing the epic

- `tidy` — correctly not run (only once, at the very end of the whole epic).
- **The `format` and `golden-update` CLion run configurations are broken** in
  the current `.idea/`: both report *"Incorrect run configuration: Executable
  is not specified"*, and `get_run_configurations` shows `golden-update` with
  no environment at all while every working configuration carries the full Nix
  dev-shell env. They have lost their target binding. Worked around this
  session with `cmake --build build/build-debug --target format` /
  `--target golden-update`, which both work fine — so this is an IDE config
  problem, not a build one. Worth repairing since `format` is a per-phase
  obligation.

Done at the end of this session: `format` (12 files), `graphify update .`
(978 nodes / 2139 edges / 83 communities), full `All CTest`, and
`golden-update` (75 golden files changed — the `If` category move indents every
conditional one level under a new `ExprStmt`, exactly the mechanical churn the
plan predicted).
