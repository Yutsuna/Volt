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

> **Every owned value lives until the end of its region, unless its ownership
> is transferred first. Every path that leaves a region crosses that region's
> cleanup boundary.**

Everything else is a consequence.

### `Owned` is proven, never presumed

The single most important decision in the model, and the one most likely to be
undone by a well-meaning change:

| Form | State | Why |
|---|---|---|
| `Identifier` / `InstanceVar` / bare `Member` | `Borrowed` | reads an existing place |
| `Call` whose resolution has `bConstructs` | `Owned` | a construction is a new value; certain, not inferred |
| `Call` whose callee has `bReturnsOwned` | `Owned` | derived by fixpoint over bodies, recorded on the resolution |
| everything else | `Borrowed` | **the safe default** |

`bReturnsOwned` is *derived and recorded on the resolution*, never an
annotation — the shape `rules/zero-hardcode.md` sanctions, and the reason the
annotation list stays closed at three.

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
typing every generic body already uses is sufficient). Element cascade, gated
on a node-kind claim so `Proc<R>` no longer receives a synthesized `.size`/`[]`
loop. Full-expression temporaries: chained rvalues, rvalue arguments, simple
and conditional moves, and temporaries live across a `raise`. Move-out
exemption per exit site.

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
corruption, 0 double frees**; the residue is leaks in 7 samples, from three
causes.

### 1. A paren-less call in nested position is never provably owned

A paren-less invocation is a bare `Member`, not a `Call` — `a.dup2 + b` parses
as `Binary( Member( a, dup2 ), b )`. `Temporaries::ResolutionOf` keys `Call` on
its callee and `Binary`/`Unary` on themselves, and does not handle `Member`, so
such a value is never proven owned and never released. This is why
`d.full_id == s`, `s.trim + t`, and the `x.to_string` an interpolation lowers
to all leak, while `s = d.full_id` does not — a *named* local is finalized by
`ScopeCleanup` on its type, needing no resolution at all.

**Adding `Member` to that group is wrong and was tried**: it converts 8 leaks
into 6 double frees. A `Member` is also how a place is read, and an owned value
reaching a field by a path the `Moved` marking does not cover is then released
twice — `Exception#capture_backtrace` stores a `String` into a field that
`Exception#finalize` frees. Closing this needs `Member` split into *invocation*
vs *place read* in the ownership model. It is the largest remaining item and
the one to do next.

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

Closure `env` is also deliberately excluded from full-expression lifetime: a
capturing closure's env holds the enclosing frame's locals, so releasing it at
the end of the statement that built it frees storage the scope still reads.
Giving `env` its true lifetime is scope-level work.

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
