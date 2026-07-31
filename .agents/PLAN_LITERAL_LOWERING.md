# Plan — `ArrayLit`/`HashLit` fully lowered before codegen (replaces `PLAN_LLVM.md` §5)

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

### 2. The rewrite: done inside `TypeChecker`, once the literal's own type is known

**The key unlock**: the rewrite itself needs *no* type information — it is
purely structural (`N` elements → `N` calls). It only needs the concrete
element type to build the initializer call and to let the synthesized `<<`
calls resolve, and it needs that fully-stabilized type only *after*
`TypeChecker` has already computed it, same as today
(`TypeCheckerConstraint.cpp:110`'s `ConstrainNode( ArrayLit )` already
re-types the literal once, after unification, exactly at this point). So the
rewrite runs **inside `TypeChecker`'s existing handling of `ArrayLit`**, right
after that constraint step, not as a separate pass and not as a new
`PassList.inl` entry — the "no node-creating pass runs after `TypeChecker`"
invariant (`MIDDLEEND.md`) is about passes *after* `TypeChecker` finishes; this
runs *as part of* it, the same way default-argument expressions are already
typed inline, in the unit that declares them, during the same pass.

Concretely, for `[ e0, e1, e2 ]` once its `SemaType` is `Array<T>` for a
concrete, resolved `T`:

1. Synthesize a fresh local slot `tmp` (an ordinary `LocalDecl`-shaped
   binding, scoped to a new `BeginExpr` wrapping the whole rewrite — see
   below for why `BeginExpr`, not a new node).
2. Synthesize `Assign{ Target: tmp, Value: Call{ … } }` where the `Call`'s
   resolution is set directly via `LookupOn( Context, LiteralSemaType,
   ConstructorCall )` (`MemberResolver.cpp:89` — **this already exists**,
   unconditionally, for any `T.new(...)`; nothing new here, matching
   `PLAN_LLVM.md` §5d's original observation that `@[LiteralInit]` was never
   needed).
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
5. `Context.Expr( Id ) = BeginExpr{ Body: [ the Assign, the N Binary
   ExprStmts, tmp as trailing value ] }` — the standard copy-out/write-back
   rewrite (`rules/ast-rewrite.md`): the source `ArrayLit` is read by value
   before any `Context.Add()`, and the assignment sequences the `Add()` calls
   before the destination write.

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
9 sugar" becomes **25 core, 11 sugar**. Update in the same change:
`core-ast.md`'s node table and its "why `ArrayLit`/`HashLit`/`CaseExpr` are
core" section (only `CaseExpr` keeps that reasoning now), `BACKEND.md`,
`MIDDLEEND.md`, and `AGENTS.md`'s "27-Node Core AST Contract" bullet.

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

1. **Rewrite site**: inside `TypeCheckerConstraint.cpp`'s
   `ConstrainNode( ArrayLit )`, immediately after it stabilizes the element
   type.
2. **Diagnostic**: none of its own. `<<`/`[]=` are ordinary methods resolved
   through the ordinary path (`MemberType`); an unresolved call fails with
   whatever generic diagnostic that path already produces for any method call.
   No custom wording, no special case.
3. **`HashLit` ships after `ArrayLit` is green**, per §6.

---

## Execution order

1. `Array<T>#<<` in `source/Lib/Primitives/Array.vl` (§1).
2. The `TypeChecker`-internal rewrite for `ArrayLit` only (§2, §3).
3. Delete the backend's `ArrayLit` dispatch arm + verify `FailAggregateLiteral`
   still exists for `HashLit` alone in the interim (§4).
4. Mark `ArrayLit` as `VOLT_EXPR_SUGAR`; update `AstInvariant`'s corpus census
   and the doc counts (§5) — for `ArrayLit` only at this point.
5. Verify: `samples/Tests/Functional/Composition.vl`,
   `ControlFlow/WhileLoop.vl`, `ControlFlow/BreakNext.vl` (the three Phase-5
   blockers named in `PROGRESS-LLVM.md`) all construct arrays with zero
   backend involvement — an ASan build, plus the `^Llvm` CTest filter.
6. Repeat 2–5 for `HashLit` (§6), including `ControlFlow/ForLoop.vl` once
   Phase 7 (`Hash#each`) also lands.
7. Doc pass: `core-ast.md`, `BACKEND.md`, `MIDDLEEND.md`, `AGENTS.md` node
   counts (§5); `PLAN_LLVM.md` §5 marked superseded, pointing here.

## Verification

- Zero survivors of `ArrayLit` (then `HashLit`) past `TypeChecker`, on two
  files differing only by padding (`rules/ast-rewrite.md`'s census
  discipline):
  ```sh
  ./build/bin/volt_d parse --lowered --no-color --no-location F | grep -cE '─ ArrayLit\b'
  ```
- An ASan build (Debug, `VOLT_ENABLE_ASAN=ON`) on the synthesized-node rewrite
  path specifically — it performs `Context.Add()` while holding onto `Id`,
  which is exactly the hazard `rules/ast-rewrite.md` exists to catch.
- `AstInvariant`, `ZeroHardcode`, `Corpus.*`, `Golden.*` and the three
  `*SerializeTest` stay green; `golden-update` after step 4/6 (the dump
  changes shape — an `ArrayLit` leaf becomes a `BeginExpr` subtree — same
  "mechanical churn, not a signal" note `PLAN_LLVM.md` Phase 8 already makes
  for the `If` move).
