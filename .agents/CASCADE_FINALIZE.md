# RAII follow-up epic: cascading `finalize()`

Four gaps identified after `fix/87-volt-lib-valgrind` closed every Exception/RAII
sample under valgrind (see that branch's own history for the RescueClause
top-level scope bug and the `ScopeOf`/`SlotFor` fix). This file tracks design
and status for each.

## 1. Rvalue / expression-temporary materialization — IN PROGRESS

`a + b + c` produces an anonymous, unnamed intermediate String (the result of
`a + b`, immediately consumed by the outer `+`). `FinalizeLowering` only ever
finalizes a *named* local declared directly in a body (`CollectCandidates`
walks `Scope.Order`/`Bindings`); an expression-tree temporary has no binding
for it to find, so it leaks. `StringBuilder` sidesteps this in the stdlib
(`Exception#report_unhandled`/`format_backtrace` now build incrementally
instead of chaining `+`) but the compiler doesn't solve it generally — any
user code chaining `+`/other Aggregate-returning operators still leaks.

Design: extend `FinalizeLowering`'s per-`StmtList` walk (or a new sweep
ahead of it) to materialize every Aggregate-typed, non-tail sub-expression of
a `Binary`/`Call` chain into a synthesized named temp (`Sema::BindingSite{ExprId}`,
the same "implicit local" shape `ClosureLifting`'s own `__env` already uses),
then finalize that temp immediately after the parent expression is done
reading it. Full-expression lifetime (C++/Rust style): the temp lives from
its own construction to the end of the enclosing statement.

Status: design only, not yet implemented.

## 2. `Array<T>#finalize` cascading into elements — BLOCKED

Needs generic type bounds (`T : Finalizable`) that Volt does not have.
`MonoDriver`/`MonoBodyEmitter` (monomorphisation) live exclusively in
`BackendLLVM/`, never in `Sema/`: a generic body like `Array<T>#finalize` is
type-checked **once**, with `T` left deferred (`MarkDeferred`) —
`FinalizeLowering` (a `TypeChecker` sweep) never sees a concrete `T` and
cannot decide "does this `T` declare `finalize`". Deciding that in the
backend would mean the backend acquiring protocol knowledge about a Volt
construct, which `backend-machine-only.md` forbids outright.

Real fix needs a language feature: generic bounds constraining `T`, checked
at the generic definition site the same way `Arithmetic`'s abstract contracts
already gate primitive operators. Out of scope until that lands.

Status: **not started, explicitly deferred** — documented here as the
blocking dependency for anyone revisiting this.

## 3. Auto field-finalize propagation for Aggregate fields — IN PROGRESS

Rust/C++-style: a struct/class's own `finalize` (whether user-written or
absent) should not need to hand-enumerate every Aggregate-typed field itself
(`Exception#finalize` currently does `@message.finalize` by hand). The
compiler should synthesize field cascade calls automatically, appended after
whatever body the user wrote (or as the entire body if the user wrote none).

Design (concrete, non-generic types only — the same generic-instantiation
wall as #2 applies to a field whose own type is a generic parameter, so this
is scoped to fields with a concrete Aggregate type):

- New step in `FinalizeLowering.cpp`, run once per `Struct`/`Class` Decl
  (`Node.Generics.IsEmpty()` only — a generic type's own field types aren't
  resolved until instantiation, same wall as #2):
  - Enumerate `Body`'s `Frontend::Field` nodes.
  - For each, resolve its declared type to a concrete `SemaTypeId` (via the
    type's own `NominalType.Members` entry — `TypeStore::OwnMember` already
    answers "declared directly, ignoring inheritance" for the exists check;
    `Member::Result` is a `SigTypeId`, converted through
    `Instantiate(Store, SigTypeId, ReceiverArgs={}, Self, Values)` since the
    struct itself is non-generic) and test `IsFinalizeCandidateType` (already
    exists, shared with the local-candidate path).
  - If the type has no cascade-candidate fields, skip entirely (silent
    default, same discipline as every other gate in this pass).
  - If it does: if the type has **no own `finalize`** (`OwnMember` check —
    inherited doesn't count, matches how a subclass with no new fields is
    left alone since it already inherits its base's finalize), synthesize a
    brand-new `Method` Decl (`finalize -> Void`, body = one
    `@field.finalize` `Call` per candidate field, reverse field-declaration
    order) and append it to `Body`.
  - If it **does** have its own `finalize`, append the same per-field
    `@field.finalize` calls to the *end* of that method's own `Body`
    (respecting whatever exits `FinalizeLowering`'s existing wrap/splice
    machinery already handles — this cascade epilogue is just more
    statements at the natural tail, so it rides the existing Step 5 wrap for
    free).

This closes the redundancy `Exception#finalize` has today: once shipped,
`Exception.vl`'s own `finalize` should shrink to just the backtrace
*element* loop (`Array<T>` itself still doesn't cascade into elements — #2 —
so that loop stays hand-written), and the `@message.finalize` /
`@backtrace.finalize` lines become auto-generated instead of hand-written —
remove them by hand once this ships to avoid a double-free.

Status: design landed, implementation starting.

## 4. `Proc.env` / closure-capture leaks — NOT STARTED

Block-arg temporaries (`arr.each do |x| ... end`) and other closure literals
lifted by `ClosureLifting` have no named local for `FinalizeLowering` to
attach to — the `Proc.new(FuncAddr, env)` construction is itself a temporary,
consumed directly by the call it's an argument to, structurally the same gap
as #1 (an rvalue temporary) but for `Proc`'s own heap-allocated env buffer
specifically (`to_address`/`from_address` `Pointer<UInt8>` arithmetic,
per `core-ast.md`'s "Closures are gone" section).

Confirmed leaking today: `Curry.vl`, `Composition.vl`, `PointFree.vl`,
`Mixins.vl`, `FullOOP.vl`, `ForLoop.vl`, `BreakNext.vl` (7/55 samples under
`scripts/valgrind_check.py`, all pre-existing, unrelated to this session's
Exception/RAII fixes).

Design: likely resolved as a *consequence* of #1 once rvalue-temporary
materialization exists (a `Proc.new(...)` call whose result isn't bound to a
name is exactly the shape #1 targets) — worth revisiting after #1 ships
rather than solved separately. If it doesn't fall out for free, `env`'s own
free needs to be spliced at the point the call consuming the `Proc` returns
(non-escaping case) or left to the receiver (escaping case — already tracked
by `ScopeTable::Escapes`/`SetEscapes`, so the information to decide this
already exists).

Status: not started, expected to piggyback on #1's mechanism.
