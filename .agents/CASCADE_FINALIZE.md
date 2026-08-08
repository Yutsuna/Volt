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

## 2. `Array<T>#finalize` cascading into elements — DONE

Turned out **not** to need generic bounds at all — the original "BLOCKED"
framing conflated two different questions. `Array<T>#finalize`'s own body
(inside `source/Lib/Primitives/Array.vl`) genuinely cannot special-case
`T` (that *would* need bounds, `T : Finalizable`, and would wrongly force
every `Array<T>` — including `Array<Int32>` — to require `T` to declare
`finalize`). But the actual leak lived one layer up: whatever local/field
*holds* the `Array<T>`. `FinalizeLowering.cpp` already resolves that
holder's own generic argument (`GetElementFinalizeCandidateType`, reading
`SemaType.Args[0]`, general to any single-generic-argument Aggregate, not
just `Array` — no Volt name spelled in C++) and, if the element type is
itself a finalize candidate (declares `finalize`, checked the same
structural way as every other candidacy in this file — duck-typed, no
bound), synthesizes a `while` loop calling `element.finalize()` on every
element (via `.size`/`[]`, both ordinary member calls resolved through
`InferExpr` — `rules/zero-hardcode.md`'s "a synthesized operator is not a
built-in either") *before* the container's own `.finalize()`. This existed
for **local** candidates already (`BuildFinalizeCall`, shipped in an earlier
session); this pass added the same cascade to **field** candidates
(`BuildFieldFinalizeCall`), by extracting the shared element-loop logic into
`BuildFinalizeCallOnReceiver` (parameterized over a `MakeReadId` callable —
an `Identifier` read for a local, an `InstanceVar` read for a field) so both
call sites stay in sync instead of drifting.

No new annotation, no bound on `Array<T>` itself — `Array<Int32>` is
untouched and still typechecks with no `finalize` requirement on `Int32`.
Verified valgrind-clean: `samples/Tests/RAII/FieldArrayElementCascade.vl`
(a `struct` field typed `Array<String>`, no user-declared `finalize` at
all — item 3's synthesis handles that half, this item's element loop
handles the elements). Confirmed no double-free between the array-typed
local passed into the constructor and the field alias it initializes (the
existing "bare-identifier-read is an alias, not a candidate" rule in
`CollectCandidates` already prevents the local from *also* being finalized
independently of the field it was moved into).

Status: **done**. The generic-bounds language feature (below, item 3's
prerequisite investigation) landed anyway as part of this epic, but this
item did not end up needing it.

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
