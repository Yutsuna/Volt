# RAII in Volt: the ownership model and what it currently reaches

Volt has no GC. The middle-end injects `finalize()` calls at region exits;
the backend gains no node for any of it (`rules/backend-machine-only.md`).
This file records the model the RAII epic settled on, and — measured, not
projected — exactly where it still stops.

It replaces the older four-item gap list. Those four items were phrased as
independent leaks; three of them turned out to be the same missing concept
(nobody owned an unnamed value), which is why the rewrite is organised around
ownership rather than around symptoms.

## The rule

> **The RAII machinery takes an object and does not care in the slightest what
> type it is. At its birth it calls the constructor; at its death it calls the
> destructor. That is all.**

There is **no special case, ever** — not for a token, not for a type, not for
a node kind. Anything that looks like one is a hardcode wearing a disguise,
and it is a bug in this file's model rather than a pragmatic exception. The
one place the compiler is allowed to be clever is *where* the destructor call
goes, which is the second rule:

> **Every owned value lives until the end of its region, unless its ownership
> is transferred first. Every path that leaves a region crosses that region's
> cleanup boundary.**

Everything else is a consequence.

### `finalize` is universal — the C++ `= default` rule

Every declared type has a `finalize`, synthesized empty by the serial seam
wherever the source does not write one, exactly as C++ gives every class a
destructor. Calling it on a type that declares none is a no-op.

That is not a convenience. It is what allows a container to release its
elements **in Volt** — `Array<T>#finalize` loops over `@buffer` and calls
`finalize` on each element, the same body `std::vector::~vector` has — instead
of the middle-end synthesizing a `.size`/`[]` loop, which it could only do by
knowing what a *sequence* is. That synthesis existed, gated on a
`@[Literal( ArrayLit )]` node-kind claim, and it is **deleted**.

Universality costs nothing because of `NominalType::bTrivialFinalize`, the
exact counterpart of `std::is_trivially_destructible`: a defaulted `finalize`
with no field to cascade and no ancestor that writes one does nothing, so no
region injects a call to it, and `Raii::IsFinalizeCandidateNominal` — now the
*only* question the sweeps ask about a type — reads that bit and nothing else.

And the bit is exposed to the language, because the library needs it for the
same reason libstdc++ does:

```volt
def finalize -> Void
  if not trivially_destructible? T   # a compile-time predicate, like `sizeof`
    i = 0_u64
    while i < @size
      ( *( @buffer + i ) ).finalize
      i += 1
    end
  end
  @buffer.free
end
```

`trivially_destructible?` is a keyword in `TokenKind.inl` and a core, inert
`TypeTrait` node — `SizeOf`'s sibling in every respect, deferred inside a
generic body and settled once per instantiation by `ReinstantiateBody`. A
second predicate (`trivially_copyable?`, `trivial?`) is one manifest row, one
label on the parser's existing arm, and one derivation; they are deliberately
not added yet, because until Volt has a customizable copy they would be
indistinguishable from this one.

### `Owned` is proven, never presumed

The single most important decision in the model, and the one most likely to be
undone by a well-meaning change:

| Form | State | Why |
|---|---|---|
| `Identifier` / `InstanceVar` / `Member` resolving to a **field** | `Borrowed` | reads an existing place |
| `Identifier` / `Member` resolving to a **method** with `bReturnsOwned` | `Owned` | a paren-less invocation is a call, not a place read |
| `Call` whose resolution has `bConstructs` | `Owned` | a construction is a new value; certain, not inferred |
| `Call` whose callee has `bReturnsOwned` | `Owned` | derived by fixpoint over bodies, recorded on the resolution |
| everything else | `Borrowed` | **the safe default** |

The first two rows are the same two node kinds: `Member` and `Identifier` mean
*either* a paren-less invocation *or* a place read, and the discriminator is
what the resolution found — `EMemberKind::Method` against `EMemberKind::Field`
— never the node kind, which cannot tell them apart. That split lives in
`Lifetime/ExprOwnership.hpp` and is shared by `Temporaries` (which asks it of
an unnamed value) and `ScopeCleanup` (which asks it of a local's initializer),
so the two cannot drift.

`bReturnsOwned` is *derived and recorded on the resolution*, never an
annotation — the shape `rules/zero-hardcode.md` sanctions, and the reason the
annotation list stays closed at three.

### An argument is only the caller's to release if the callee borrows it

`Member::ParamEscapes`, derived per parameter by the same seam
(`Raii::InferParameterEscape`), and the mirror image of `bReturnsOwned`:
ownership of a *result* must be **proven** before the caller releases it, so
that fixpoint starts `false` and climbs; escape of an *argument* must be
**disproven** before the caller releases it, so this one starts `true` and
falls. Both leave the unprovable case on the leak side.

Without it, `arr.push( s.dup )` is a **double free**, not a leak: the region
releases the temporary, `Array<T>#push` stored it, and the array's own element
loop releases it again. That was a real, pre-existing defect, and closing it
is what made classifying a paren-less `Member` as an invocation safe at all.

The same bit makes `ScopeCleanup`'s escape check precise: it used to refuse
candidacy for any bare-name argument of a *constructor* — the only scope in
which it did not produce false positives — and now asks the narrower question
of every call.

The asymmetry is deliberate and load-bearing: **a missed classification costs a
leak, a wrong one costs a double free.** A leak is a counted, bounded gap; a
double free is memory corruption. Whenever the two trade off, the model takes
the leak. Section "What is still open" is entirely made of leaks for this
reason, and that is the intended state, not a backlog of equal-severity bugs.

### Regions

`CleanupRegion` is the primitive; `BeginExpr { Body, EnsureBody }` is merely
how it is lowered today. **Only `CleanupRegion.cpp` may construct a
`Frontend::BeginExpr`** — that is what keeps "give regions a different
representation" a one-file change, and it is checkable:

```sh
grep -rn "BeginExpr" source/Volt/Sema/Private/Passes/TypeChecker/Lifetime \
                     source/Volt/Sema/Private/Raii     # → CleanupRegion.cpp only
```

Two kinds, one implementation: a **scope region** (a `StmtList`) owns its named
locals; a **full-expression region** (a statement that materialized
temporaries) owns those. Each emits exactly one boundary regardless of how many
values it holds.

### Exits

Two mechanisms, because the backend already treats the two differently:

- **fall-through, unhandled `raise`, non-local `break`** — wrapped, and
  `EmitBegin` threads all three through `EnsureBody` with no new node;
- **`return`, loop-owned `break`/`next`** — all three bypass `Ensure` entirely
  in the backend, so the finalize calls are *spliced* directly before the exit.

Discovery is uniform since Phase 5: `CollectNestedBlockExprs` finds a nested
block by *where it sits*, not by which statement encloses it, so an exit in
expression position (`x = if c then return 1 else 2 end`) reaches the same
recursion a statement-position one does. There is no per-method bail-out any
more, and `RaiiUnsupportedExits` is incremented nowhere.

## What works

Field cascade, including on **generic** types (`Hash<K, V>` cascades into
`@entries : Array<HashEntry<K, V>>` with no bound on `K`/`V` — the deferred
typing every generic body already uses is sufficient). Element release, now
written in Volt by the container itself rather than synthesized — see the
universal-`finalize` rule above; the node-kind-gated `.size`/`[]` loop that
used to live in `FinalizeCallBuilder` is gone. Full-expression temporaries:
chained rvalues, rvalue arguments, simple and conditional moves, and
temporaries live across a `raise`. Move-out exemption per exit site.
Per-parameter argument escape, so a callee that stores what it is handed is
never double-freed by its caller.

Fixtures: `samples/Tests/RAII/`, each with a `.expected`. The seven locked
shapes are `TempChainedRvalue`, `TempAsArgument`, `TempSimpleMove`,
`TempConditionalMove`, `ExpressionPositionReturn`, `ExpressionPositionBreakNext`,
`TempRaiseDuringBuild`, plus `GenericHashCascade`.

**The `.lowered.golden` files do not cover any of this.** `parse --lowered`
runs only `EPassKind::Lowering` passes, and the whole RAII sweep runs *inside*
`TypeChecker`, so a lowered golden of a RAII sample shows the sweep's input,
never its output — `StraightLineSingle.vl.lowered.golden` contains zero
`finalize`. They are kept as ordinary lowering-pass coverage. The real oracles
are the `.expected` execution tests and `scripts/valgrind_check.py`.

## What is still open

Measured with `scripts/valgrind_check.py`: **57/65 passed, 0 memory
corruption, 0 double frees**; the residue is leaks in 8 samples, from three
causes. (`valgrind_check.py` labels them MEMORY ERROR rather than LEAKED
because `--leak-check=full` counts a definite leak as an error, so
`--error-exitcode` fires first — the script's own status ordering, not
corruption.)

**The universal-`finalize` work of `.agents/PLAN_RAII.md` §Phase 7 compiles
but has not been measured against this baseline.** Read that section before
trusting any number here.

### 1. A paren-less call in nested position — split done, leaks still open

The split this item asked for has landed: `Member`/`Identifier` are now
classified as invocation or place read from the resolution's `EMemberKind`,
and the double frees that made the naive widening impossible (`arr.push(
s.dup )`, `Exception#capture_backtrace`) are closed by `ParamEscapes`.

**The leaks are nevertheless unchanged**, and the reason is not this item's:
`bReturnsOwned` cannot prove a body it cannot read. `ServiceDevice#full_id` is
`"#{ id_prefix }#{ @serial }"`, and the seam-time fixpoint runs *before*
`InterpLowering`, so it sees one `Interp` node instead of the `String#+` fold
it becomes. Every method whose body is an interpolation is therefore
classified `Borrowed`, and its result leaks at every call site.

Teaching the fixpoint what an interpolation lowers to was tried and is
**forbidden**: it puts a Volt method spelling (`"to_string"`) in the
middle-end. The fix is to move the seam after `EPassKind::Lowering` so the
fixpoint reads the core AST and resolutions instead of spellings — Road A in
`.agents/PLAN_RAII.md` §Phase 7, which also deletes the name index,
`TokenSpelling` and `"new"` from this analysis.

### 2. Reassigning an owned local abandons the old value

`result = n.digit_char + result` in `Stringable#to_string` overwrites a local
that owns a buffer; the previous buffer is neither moved nor finalized, so it
leaks once per iteration (`5.to_string` is clean, `42.to_string` leaks one
block). Drop-on-reassign needs flow-sensitive per-local ownership: a local
initialized from a borrow must *not* be released on reassignment, so a naive
"release before store" is a double free. This is genuinely the same
flow-sensitivity the conditional-move design already reasons about, and belongs
with item 1.

### 3. An indirect call's result cannot be classified

`f( 1 )( 2 )` calls through a `Proc` value; the resolution carries no `Decl`,
so the returned closure's env is `Borrowed` and leaks. Unlike 1 and 2 this is
not an oversight — through a function pointer the body is genuinely unknown.
It needs either escape analysis or an ownership bit on the callable *type*.

The **block-argument** half of this, which is most of the measured bytes, is
not that wall and has a safe answer, designed and not yet written.
`ClosureLifting` builds the region itself:

```
BeginExpr {
  Body:   [ __env = malloc( n ), copy captures in, parentCall( …, block ) ]
  Ensure: [ copy captures back ]
}
```

so appending `__env.free` to the **end of that `EnsureBody`** is correctly
ordered on both the normal and the unwind path, and is guarded by
`Raii::BlockParameterEscapes` on the parent call's resolution — if the callee
could store the block, nothing is freed. That closes the env leaks in
`ForLoop` (×4), `GenericHashCascade`, `BreakNext`, `Composition` (×2) and
`PointFree`.

**Do not remove `Temporaries`' `IsCallableType` guard to get there.** A
composition (`(&.trim) >> (&.downcase)`) captures the inner `Proc`s *by value
into its own env*, so releasing an intermediate `Proc` temporary frees an env
the composed closure still calls through. Closure `env` therefore stays out of
full-expression lifetime; a `Proc` *bound to a name* is already released
correctly by `ScopeCleanup`, since `Proc#finalize` frees `@env`.

## Accounting, not valgrind

The intended long-term check is an identity, not a memory tool:

```
RaiiOwnedCreated == RaiiMoves + RaiiFinalizes + RaiiExplicitEscapes
RaiiOwnedWithoutCleanup == 0
```

Counters live on `PassStats`, which `Meta::ForEachField` walks, so a new one is
one field and reaches `volt check --metrics` with no other edit. Note that
`RaiiLocals` is currently declared but incremented nowhere — the live counters
are `RaiiTemporaries`, `RaiiCleanupPaths`, and `RaiiNestedExpressionExits`.

## The guardrail this file exists to protect

One `constexpr std::string_view StringifyMethod = "to_string"` was written
into `Raii/OwnershipInference.cpp` during Phase 6, to let the fixpoint see
through an interpolation. It was removed the same day. Record it here rather
than quietly delete it, because the pressure that produced it is structural
and will produce it again:

> Any time this analysis has to *guess what a node will lower to*, it is being
> asked a question from the wrong place in the pipeline. The answer is to move
> the question, never to teach it a spelling.

The checks that catch a relapse, from `rules/zero-hardcode.md`:

```sh
grep -RnE '\b(String|Array|Int32|Int64|UInt8|Float64|Proc|Exception)\b' \
  source/Volt/Frontend source/Volt/Sema --include='*.hpp' --include='*.cpp'
grep -RhoE '@\[[A-Za-z]+' source/Lib | sort -u   # @[External @[Literal @[Primitive
```

and, specific to this subsystem — no member spelling and no token in the RAII
tree beyond the constructor/destructor protocol itself:

```sh
grep -nE '"[a-z_]+"|TokenSpelling' \
  source/Volt/Sema/Private/Raii/*.{hpp,cpp} \
  source/Volt/Sema/Private/Passes/TypeChecker/Lifetime/*.{hpp,cpp}
```
