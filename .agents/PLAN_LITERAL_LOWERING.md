# Plan — `ArrayLit`/`HashLit` fully lowered before codegen (replaces `PLAN_LLVM.md` §5)

## Status

- **`ArrayLit`: done.** `Array<T>#<<` (§1), the lowering (§2 — corrected
  below, it runs as a post-walk sweep, not inline), the backend arm deleted
  (§4), `VOLT_EXPR_SUGAR` (§5). Verified: a real build, `check` across
  `samples/`, an ASan build clean across the same corpus, and the two bugs
  §2's original "inline" design had (below) reproduced and fixed.
- **`HashLit`: done.** Lowering via `LowerHashLits` post-walk sweep in `TypeChecker`, `[]=` resolved via ordinary `MemberType`, backend arm deleted, `FailAggregateLiteral` deleted, `VOLT_EXPR_SUGAR`.

## Why §5 of `PLAN_LLVM.md` is wrong

The original Phase 5 design (§5a–§5f) kept `ArrayLit`/`HashLit` as **core**
nodes that the backend itself dispatches on and constructs — via
`@[LiteralAppend]` (a 4th annotation), a widened `UnitCallees` the backend
reads directly, and a backend-side `EmitArrayLit`/`EmitHashLit`. Two decisions
made this session overturn that:

1. `@[LiteralAppend]` is refused — `rules/zero-hardcode.md`'s closed
   annotation list is exactly `@[Primitive]`, `@[External]`, `@[Literal]`; see
   [[zero-hardcode-annotations]] and the "refused example, kept on purpose"
   section already in that file.
2. **The backend may not know `ArrayLit`/`HashLit` exist at all**, in any
   form — not even a "thin" handler that just reads a resolved `CalleeEntry`
   and calls `EmitResolvedCall`. See the new `rules/backend-machine-only.md`.
   A first attempt at a backend-side `EmitArrayLit` that built the aggregate
   by hand (`malloc` + shape-guessing which struct field is the buffer) was
   tried in-session and correctly rejected: it doesn't hardcode a Volt type
   *name*, but it hardcodes Array's *structure* (one pointer field is a
   buffer of N elements; both integer fields equal N for a fresh literal) —
   the same disease as the annotation, in a different C++ form. It also
   doesn't generalise: `Hash`'s own struct (`{ entries: Array<HashEntry<K,V>>,
   size: UInt64 }`) has no buffer field at all, so the trick breaks on the
   very next literal kind.

**Conclusion**: `ArrayLit`/`HashLit` must be genuinely eliminated — lowered
into ordinary already-typed core AST — before any backend walks the tree.
`rules/core-ast.md`'s classification of them as "core, not sugar" (reasoned as
"lowering needs the generic argument, which is only known after
`TypeChecker`, and no pass may create untyped nodes after `TypeChecker`") is
**not actually a wall**, once the rewrite is done at the right point and in
the right shape — see below.

---

## The corrected design

### 1. Stdlib: declare the append operator, in Volt, with a real body

- `source/Lib/Primitives/Array.vl` — add `def <<( value : T ) -> self` next to
  `push`, calling it (`push( value ); self`). **One new line class**, not a
  new mechanism: `<<` is already a real token (`TokenKind::Shl`,
  `TokenKind.inl`), already declared as an *abstract* operator on
  `mixin Arithmetic` (bitshift, on primitives) — `Array<T>` doesn't include
  `Arithmetic`, so there is no collision, exactly the way Ruby overloads `<<`
  for both `Integer#<<` (shift) and `Array#<<` (push) with the same token.
- `source/Lib/Primitives/Hash.vl` — **no change**. `def []=( key : K, value :
  V ) -> Void` already exists and already is the append operation for a
  literal (`{ "a" => 1 }` needs exactly `h["a"] = 1`).
- **This declaration is not optional plumbing** — it is what gives the
  rewrite below somewhere real to resolve to. Exactly as `Int32 + Int32`
  stops typechecking the moment `Int32` drops `include Arithmetic` (`+` has
  no meaning to the compiler beyond "whatever member with that spelling this
  type declares"), a type that claims `@[Literal( ArrayLit )]` but declares no
  `<<` fails the rewrite with an ordinary "no member" diagnostic — the
  rewrite gets no shortcut past member resolution, per
  `rules/backend-machine-only.md`.

### 2. The rewrite: a post-walk sweep inside `TypeChecker`, once every constraint in the file has settled

**Corrected from the original design below** (kept for the record — it was
implemented, and broke on real code): the rewrite does not run inline, inside
`ConstrainNode( ArrayLit )` or `ComputeExpr`'s `ArrayLit` branch, the moment
either first computes a type for the literal. It runs **once**, in a single
post-walk arena sweep (`LowerArrayLits`, `LiteralLowering.cpp`), called from
`TypeChecker.cpp` right after `WalkDecls` finishes and before the
`CalleeResolution` snapshot — the same shape `RejectNilableTypes` already uses
in that file, over the same `PassContext`.

**Why inline doesn't work**: a literal's *own* type can be settled by two
different call sites that do not run in the order you'd expect. `CallType`
(`ExprInferencer.cpp`) infers every argument's natural, bottom-up type
*before* `CheckCallArgs` ever gets to push the parameter's declared type down
through `ConstrainExprType` — its own comment says so: "arguments are bound
before being checked". So `sum_all( [ 4, 5, 6 ] )` against a declared
`sum_all( values : Array<UInt64> )` naturally infers `[4, 5, 6]` as
`Array<Int32>` first; only afterward does `CheckCallArgs` try to narrow it to
`Array<UInt64>`. Rewriting inline the instant *either* call site first
computes a type means the rewrite fires on the *first* one — the wrong one,
here — and permanently bakes `Int32` elements into the synthesized `<<` calls
before the real target type ever arrives. `samples/Sema/UnconstrainedLiterals.vl`'s
`sum_all( [ 4, 5, 6 ] )` is exactly this case and is what caught it: it
type-checked with a wrong-generic-argument error ("argument 1 to sum_all has
type Array, expected Array" — `NameOfValue` prints only the base nominal, so
the message doesn't even show the mismatched argument) once the inline design
was actually run against the corpus, not just reasoned about.

**The fix**: never rewrite until every `ConstrainExprType` call in the file
has already had its say. `ConstrainNode( ArrayLit )` and the literal's
ordinary bottom-up inference (`LiteralType`, unchanged, no special case)
still only *type* the literal — same code as before this plan — and the AST
node is left as an ordinary `ArrayLit` throughout the whole per-file walk.
Only the final sweep, reading each surviving `ArrayLit`'s *settled*
`Values.ExprType( Id )`, performs the actual rewrite. This also fixes a
correctness question the original design didn't have an answer for at all:
what if a literal is narrowed a *second* time after an initial inline
rewrite already ran? With the sweep, there is no "second time" — there is
exactly one rewrite, after everything else is done.

**A second bug the sweep incidentally exposed and also fixed**: once *any*
node inside `TypeChecker` can `Add()` — which was never true before this
plan — every existing reference bound straight into a live arena slot during
the walk becomes a latent use-after-free, not just the ArrayLit-specific
code this plan added. ASan caught one: `WalkStmt`'s `LocalDecl`/`Return`
handlers (`DeclStmtWalker.cpp`) visit `Context.Ctx.Ast.Stmt( Id )` *by
reference*, then read several of that node's fields interleaved with
`InferExpr` calls — one of which, for a `LocalDecl` whose `Init` is an
`ArrayLit`s, can indirectly reach the sweep's own `Add()` calls nowhere near
by textual position but very much within the same call stack once nested
constraint propagation is considered. Fixed by copying the `StmtNode` out by
value before dispatch (`DeclStmtWalker.cpp`, matching the copy `ComputeExpr`
already took for the identical reason on the Expr side), and by making
`TrailingType` (`ClosureInferencer.hpp/.cpp`) take its `StmtList` by value
rather than by reference, since a `RescueClause`'s `Body` is itself read
through a live reference into the Stmt arena. Both fixes are general — they
protect every future `Add()`-performing rewrite inside `TypeChecker`, not
only this one.

The rewrite itself is still purely structural and still needs no type
information beyond the one already-settled `SemaTypeId` it's handed — only
*when* it runs changed, not *what* it does. Concretely, for `[ e0, e1, e2 ]`
once its final `SemaType` is `Array<T>` for a concrete, resolved `T`:

1. Synthesize a fresh local `tmp` (`Ast.MakeUniqueSymbol( "__array_lit" )` —
   already exists for exactly this, nothing new). Its declaration is an
   ordinary `Assign{ Target: Identifier( tmp ), Value: Call{ … } }`, the same
   shape a hand-written `tmp = Array.new()` parses to: `WriteLocal`'s
   name-keyed `Locals` map fallback ("a node minted after Order 10",
   `TypeCheckerContext.cpp`) is what makes an Identifier ScopeResolver never
   bound resolve correctly on every later reference.
2. The `Call`'s receiver is a synthesized `Identifier` whose `SemaTypeId` is
   stamped *directly* to the literal's own (already fully-instantiated, with
   concrete generic args) type, rather than rebuilt from a source-level
   `GenericInst` — `Array<T>.new()` written by hand needs the surface syntax
   to re-derive `T`; here `T` is already known, so re-deriving it would be
   pure ceremony. Marked into `NakedTypeExprs` too, so `MemberType`'s naked-
   type checks behave identically to a real `T.new()`. Ordinary `InferExpr`
   (via `WalkStmt`) then resolves `new`/`initialize` on that receiver exactly
   as any hand-written `T.new(...)` would (`MemberResolver.cpp`'s existing
   `ConstructorCall` fallback — nothing new, matching `PLAN_LLVM.md` §5d's
   original observation that `@[LiteralInit]` was never needed).
3. For each element `e_i`, synthesize `Binary{ Op: TokenKind::Shl, Lhs: tmp,
   Rhs: e_i }` as an `ExprStmt`. Resolve it through the **same** path any
   hand-written `tmp << element` would take — `MemberType`'s ordinary
   operator resolution (`rules/backend-machine-only.md`'s "a synthesized
   operator is not a built-in either": no shortcut past member resolution).
   `<<` is a method like any other; if the type claiming `ArrayLit` declares
   no `<<`, this fails with whatever generic "no member" diagnostic
   `MemberType` already produces for any unresolved call — no custom wording,
   no special case. Record the resolution the same way `MemberType` does
   (`Context.CalleeResolution[Id.Value]` via `UnitCallees::Set`) so the
   (now-ordinary) `Binary` node is indistinguishable, from every downstream
   consumer's point of view, from one a user actually wrote.
4. The tail expression is `tmp`.
5. Every synthesized statement is typed (`WalkStmt`) off a *local* copy of
   the `Body` list being built, never off a re-read of `Id`'s own arena slot
   — then, only once every `Add()` this rewrite performs is done,
   `Ast.Expr( Id ) = BeginExpr{ Body, ... }` writes the slot
   (`LiteralLowering.cpp`'s `LowerArrayLit`). Copy-out / compute / write-back,
   `rules/ast-rewrite.md`'s canonical shape.

**Why `BeginExpr` and not a new node**: it is already a *core*, non-sugar node
every backend already emits (`EmitBegin`/`EmitCase`/`EmitIf` all converge by
slot, per Phase 2's `LoadConverged`), it already carries exactly "a sequence
of statements converging to a tail value", and it needs zero new backend
code — this is precisely the point: the backend gains *nothing* to learn,
because nothing new reaches it.

### 3. Why this works unmodified inside a generic body

An `ArrayLit` written inside `Enumerable<T>#to_a`, say, has a *deferred*
element type (`T`, not concrete) until monomorphisation. The rewrite above
does not need that to be concrete to run: it needs to know only "this is an
`ArrayLit` with N elements", which is true unconditionally. The synthesized
`Assign`/`Binary`/`Call` nodes are marked deferred exactly the way any other
expression inside a generic body already is (`UnitTypes::MarkDeferred`), and
their `CalleeResolution` is left for `Reinstantiate` to fill in per
instantiation — **the same path any other generic-body method call already
takes** (`arr.push( x )` inside a generic `Enumerable` method is already
resolved this way today). Nothing new is needed for the generic case; it was
never actually blocked by "the argument is only known after `TypeChecker`" —
that reasoning conflated *performing the rewrite* (which needs nothing) with
*knowing the concrete type* (which the existing deferred/monomorphisation
machinery already handles for every other expression shape).

### 4. Backend: nothing. `FailAggregateLiteral` and its two dispatch arms are deleted

Once §2 lands, `ArrayLit`/`HashLit` never survive past `TypeChecker` — the
backend's `std::visit` in `ExprEmitter.cpp` loses its `ArrayLit`/`HashLit`
arms entirely (not "made thin" — removed), and `FailAggregateLiteral` is
deleted along with them.

### 5. `Nodes.inl` / `AstInvariant`: `ArrayLit`/`HashLit` become `VOLT_EXPR_SUGAR`

One-line manifest change each (`rules/meta-first.md`): `VOLT_EXPR( ArrayLit )`
→ `VOLT_EXPR_SUGAR( ArrayLit )`, same for `HashLit`. `AstInvariant`'s
generated sugar-census set picks them up for free — the same machinery that
already checks the 9 existing sugar kinds now checks 11, with no code change
to `AstInvariant.cpp` itself.

**Consequence for the node count**: `core-ast.md`'s "36 `VOLT_EXPR` — 27 core,
9 sugar" is now **26 core, 10 sugar** with `ArrayLit` alone moved (done, this
change) — it becomes 25 core / 11 sugar once `HashLit` moves too. Update in
the same change as each: `core-ast.md`'s node table and its "why
`ArrayLit`/`HashLit`/`CaseExpr` are core" section (`ArrayLit`'s own reasoning
there is now marked superseded, `HashLit`'s is not — yet), `BACKEND.md`,
`MIDDLEEND.md`, and `AGENTS.md`'s "27-Node Core AST Contract" bullet — the
latter three are still pending, see the execution-order checklist below.

### 6. `HashLit` — same idea, shipped second

`HashLit`'s rewrite is the same shape (`tmp = Hash<K,V>.new(); tmp[k0] = v0;
tmp[k1] = v1; …; tmp`), using the *existing* `[]=` — but the literal's own
type resolution triggers a two-level generic instantiation
(`Array<HashEntry<K,V>>` inside `Hash<K,V>`, then `HashEntry<K,V>.new` inside
`Hash#[]=`), which needs verifying the `Monomorphizer` queue drains correctly
before relying on it. Ship `ArrayLit` first, confirm green, then `HashLit`
(same ordering `PLAN_LLVM.md` §5e already recommended).

---

## Decisions (settled)

1. **Rewrite site**: originally decided as inline, inside
   `ConstrainNode( ArrayLit )` — **superseded by §2's correction above**: a
   post-walk sweep (`LowerArrayLits`), called once from `TypeChecker.cpp`
   after `WalkDecls`.
2. **Diagnostic**: none of its own. `<<`/`[]=` are ordinary methods resolved
   through the ordinary path (`MemberType`); an unresolved call fails with
   whatever generic diagnostic that path already produces for any method call.
   No custom wording, no special case.
3. **`HashLit` ships after `ArrayLit` is green**, per §6.

---

## Execution order

1. ~~`Array<T>#<<` in `source/Lib/Primitives/Array.vl` (§1).~~ Done.
2. ~~The `TypeChecker`-internal rewrite for `ArrayLit` only (§2, §3).~~ Done,
   as the post-walk sweep, not the originally-planned inline call.
3. ~~Delete the backend's `ArrayLit` dispatch arm; `FailAggregateLiteral`
   still exists for `HashLit` alone in the interim (§4).~~ Done.
4. ~~Mark `ArrayLit` as `VOLT_EXPR_SUGAR`; doc counts (§5) — for `ArrayLit`
   only at this point.~~ Done (see below for the current count).
5. ~~Verify: a real build, `check` across `samples/`, an ASan build clean
   across the same corpus.~~ Done. (The originally-planned
   `parse --lowered` / `^Llvm` CTest verification below does not apply here:
   `--lowered` runs only `EPassKind::Lowering` passes, and `TypeChecker` is
   `EPassKind::Analysis` — the rewrite is invisible to that flag by
   construction. `check` is what exercises it.)
6. **Not started.** Repeat 1–5 for `HashLit` (§6), including
   `samples/Tests/ControlFlow/ForLoop.vl` once `Hash#each` also lands
   (`core-ast.md`'s own documented gap).
7. Doc pass: this file (done, this edit); `core-ast.md` (done, this edit —
   node table, node counts, the "why core not sugar" section); `AGENTS.md`,
   `BACKEND.md`, `MIDDLEEND.md` node-count mentions — **still pending**,
   grep for `27 core`/`27-Node`/`9 sugar`/`36 \`VOLT_EXPR\`` across `.agents/`
   before calling `HashLit` done, since the count moves a second time then.

## Verification (as actually run — `ArrayLit` only)

- `check` across `samples/`, and against a version of the sample with
  `Array<T>#<<` deleted (confirms the rewrite resolves through the ordinary
  member-resolution path, no shortcut — "type Array has no member '<<'",
  same wording any unresolved method call gets).
- A meson build with `enable_asan=true`, `checked_ids=true`, run against the
  same corpus: exercises exactly the `Context.Add()`-while-a-reference-is-
  live hazard `rules/ast-rewrite.md` exists to catch — this is what caught
  the `WalkStmt`/`TrailingType` bug described in §2.
- `samples/Sema/UnconstrainedLiterals.vl` is the regression fixture for §2's
  ordering bug (`sum_all( [ 4, 5, 6 ] )` against a declared
  `Array<UInt64>` parameter) — green.
- **Not run this pass**: `AstInvariant`/`ZeroHardcode`/`Corpus.*`/`Golden.*`
  as CTest targets, or `golden-update` — this repo's test suite has not yet
  been migrated to the current Meson build (`enable_testing` option exists,
  marked "not yet migrated"). Re-run once that lands.
