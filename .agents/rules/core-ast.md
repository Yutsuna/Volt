# Rule: core AST — the contract a backend consumes

A backend is written by declarative pattern-matching over a **core AST**. This
file is that contract: what a backend may be handed, and what it may assume.
Counting only the nodes this file tracks (it does not yet cover `TypedExpr`/
`If`, added by unrelated work — see below), `Nodes.inl` declares **25 core,
13 sugar** (was 24/13, and 25/11 before that; `Lambda` and `Block` both moved
to sugar once their construction protocol was fully lowered into ordinary AST,
and `FuncAddr` was added as a new core node in the same effort — see "Closures
are gone" below. `TypeTrait`, the compile-time type predicate `SizeOf`'s
arrival is modelled on, is the most recent core addition).

Two invariants make the contract mechanical rather than aspirational, and
`AstInvariant` (order 40) checks both on every build:

1. **No sugar survives `Lowering`.** The 13 nodes below are gone from the
   arena by the time `TypeChecker` runs.
2. **Every expression in value position has a type.** `Values.Has( Id )` holds
   for it.

**Known doc gap, pre-existing and unrelated to this section:** `Nodes.inl`
also declares `TypedExpr` (an explicit-type expression, `( 10 : Int32 )`) and
`If` as expression forms; neither is documented in this file yet — both
predate the closure-lowering epic and are a separate piece of doc debt.

## The 13 sugar nodes

`Interp` · `Index` · `DotCall` · `Section` · `Composition` · `Pipeline` ·
`JsxElement` · `JsxFragment` · `JsxText` · `ArrayLit` · `HashLit` · `Lambda` ·
`Block`

They are marked `VOLT_EXPR_SUGAR` in `Frontend/AST/Nodes.inl` — one line each,
and that mark is what `AstInvariant` reads. Adding a sugar node is one line;
forgetting to lower it is a build error, not a discovery made in the backend.

`ArrayLit`/`HashLit` are not lowered by a `Lowering`-kind pass like most of the
others — `LowerArrayLits`/`LowerHashLits` each run as a post-walk sweep
*inside* `TypeChecker` itself (`EPassKind::Analysis`), once every constraint
in the file has settled. See `.agents/PLAN_LITERAL_LOWERING.md` §2/§6 for why
(a real ordering bug forced this away from the simpler "rewrite it inline the
moment TypeChecker" design the `VOLT_EXPR_SUGAR` machinery would suggest).
`Lambda`/`Block` follow the identical shape for the identical reason —
`ClosureLifting` (`Sema/.../TypeChecker/ClosureLifting.cpp`) runs inside
`TypeChecker` too, once a closure literal's own `ClosureType` has settled, for
a two-part reason: registering the lifted function anywhere before the
pre-`TypeChecker` seam would race across units' parallel `TypeChecker` runs
(every other `Lowering` pass runs there safely only because it creates no new
top-level declaration), and an unannotated closure parameter
(`arr.each do |i| … end`) has no type before `TypeChecker` assigns one either.

A sixth post-walk sweep, `InsertFinalizeCalls`
(`Sema/.../TypeChecker/FinalizeLowering.cpp`), runs last, right after
`ClosureLifting` and before `TypeChecker`'s own `Context.Callees` snapshot
loop. It does not lower a sugar node — every node it touches is already core
— so it introduces no new `VOLT_EXPR_SUGAR` entry; it is here because it
needs the same "final type only" guarantee the other five sweeps do. A
scope-local whose resolved type has `LayoutKind::Aggregate` and declares a
member named `finalize` gets a synthesized `Call` to it inserted at every
exit from the `StmtList` it is declared directly in — the method or closure
body itself, or any nested `If`/`While`/`CaseExpr`-clause/`BeginExpr` body,
processed recursively, innermost first. Two exit shapes, both expressible
with ordinary nodes a backend already knows:

- **Fall-through, an unhandled `raise`, or a non-local `break`** — the
  `StmtList` is wrapped in a synthetic `BeginExpr{ Body, EnsureBody }`;
  `EmitBegin` already threads all three through `EnsureBody` with no new
  backend node.
- **`return`, and a loop-owned `break`/`next`** — all three bypass `Ensure`
  entirely (`EmitReturn`/`EmitBreak`/`EmitNext` in `StmtReturnBreakNext.cpp`
  branch directly, no ensure-stack lookup), so the finalize `Call`s are
  spliced directly before the exit statement instead, covering not just that
  `StmtList`'s own candidates but every enclosing scope's candidates the exit
  is also unwinding past (an "ambient" candidate list threaded down through
  the recursion — reset at each `While::Body` boundary for `break`/`next`,
  which only unwind to their own innermost loop, but carried unbounded for
  `return`, which unwinds the whole call).

A local whose exit hands it back by bare `Identifier` name is exempted at
that exit site only (ownership moves to the caller's scope).

An exit hiding inside an *expression-position* control construct
(`x = if c then return 1 else 2 end`, reachable only because `If`/`CaseExpr`/
`BeginExpr` are core *expression* nodes, not just statement-position sugar)
used to make the sweep leave the whole method untouched, rather than risk
emitting a partial set of finalize calls. It no longer does. The bail-out is
gone: the sweep finds a nested block through `CollectNestedBlockExprs`, which
follows a node's *expression* fields reflectively and reports every
`If`/`CaseExpr`/`BeginExpr` it reaches at any depth, so discovery is by where
the block sits rather than by which statement encloses it. Insertion was
already correct for these — `If::Then`/`Else` are ordinary `StmtList`s — so
only discovery had been missing. `samples/Tests/RAII/ExpressionPositionReturn.vl`
and `ExpressionPositionBreakNext.vl` are the fixtures.

Two things the sweep still does **not** own, both leaks rather than
corruption, and both recorded in `.agents/CASCADE_FINALIZE.md` with the
measurement: a paren-less call in nested position (`d.full_id == s`), and
reassignment of a local that already owns a value (`result = f( result )`
abandons the old buffer). The first one's `Member`-into-invocation-vs-place
split has since landed (`Lifetime/ExprOwnership.hpp`), but its leaks are still
open for an unrelated reason — the ownership fixpoint runs before the lowering
passes and so cannot read a body written as an interpolation; the second still
needs per-local flow-sensitive ownership. The naive widening of either
produces double frees, which the model ranks strictly worse than the leak.

## The 25 core nodes

| Category | Nodes | What a backend does with it |
|---|---|---|
| Terminals (7) | `IntLiteral` `FloatLiteral` `StringLiteral` `CharLiteral` `BoolLiteral` `NilLiteral` `SymbolLiteral` | materialise a constant (`StringLiteral` produces a raw byte buffer pointer `Pointer<UInt8>` / `u8*`, while the aggregate `String` construction `String.new( bytes, size )` is lowered upstream in `TypeChecker` by `LowerStringLits`) |
| Access (6) | `Identifier` `InstanceVar` `SelfExpr` `SuperExpr` `Member` `Deref` | load / GEP |
| Operations (3) | `Call` `Assign` `Ternary` | call through `CalleeResolution`, store, select/branch |
| Operators (2) | `Binary` `Unary` | see below |
| Control (3) | `CaseExpr` `BeginExpr` `RaiseExpr` | test chain / jump table; EH |
| Inert (4) | `GenericInst` `SizeOf` `FuncAddr` `TypeTrait` | carry no runtime value beyond an address: read `Values.Get( Id )` (resp. the layout size, resp. the resolved callable's address, resp. one settled bit of the type record) and **never descend into them** |

`TypeTrait` is `SizeOf`'s sibling: `trivially_destructible? T` names a type
rather than a value, TypeChecker publishes the *resolved* operand type on the
node's own site, and the backend materialises a constant from it — for
`SizeOf` a width from `LayoutEngine`, for `TypeTrait` an `i1` from
`TypeStore::IsTriviallyDestructible`. Inside a generic body both are deferred
and settled once per instantiation by `Sema::ReinstantiateBody`. The predicate
exists so a container can release its elements in ordinary Volt without paying
for a `T` that has nothing to release — the compiler used to synthesize that
loop itself, which meant knowing what a sequence is
(`.agents/CASCADE_FINALIZE.md`).

`Lambda`/`Block` are gone from this table — see "Closures are gone" below.
`FuncAddr` is new here: it denotes a resolved callable's address as a value,
distinct from calling it — the third meaning an `Identifier`/`Member`
occurrence can carry, alongside "read a place" and "paren-less call."

## Closures are gone — `Lambda`/`Block` are lowered, not emitted

Unlike `CaseExpr` below, closures were **not** kept core because lowering them
would cost more — the opposite held. `ClosureLifting` rewrites every
`Lambda`/`Block` literal, no-capture or capturing, into a synthesized
top-level function (registered in a per-unit `Sema::SynthesizedFunctions`
table, never in the cross-unit `TypeStore` — nothing outside the unit ever
needs to name a lifted closure by symbol, since it is only ever reached
through its own `{code,env}` pair) plus an ordinary `Proc.new( FuncAddr, env )`
construction at the literal's own site.
A capturing closure's env is `Pointer<UInt8>` arithmetic
(`to_address`/`from_address`, machine primitives in the closed vocabulary),
and every captured-variable use inside the lifted body is rewritten in place
into an ordinary `Deref`/`Call` load. The result: a backend sees a `Call` to
a synthesized function and an ordinary `Proc` value — no closure-literal
node, no nested-frame emission, no capture-binding logic of its own.
`BackendLLVM/.../ClosureEmitter.cpp` shrank from 454 to 175 lines; the
survivors (`EmitIndirectCall`, `EmitBlockNext`) are not closure-literal
machinery — they operate on an already-resolved `{code,env}` *value*, exactly
as `IsCallableType`/`bIndirect` already did before this epic (see
`backend/llvm.md`'s "Invoking one" section).

### Why `CaseExpr` is core, not sugar

It is not sugar being tolerated; lowering it would *cost* more than keeping
it. `CaseExpr` after `CaseLowering` (order 22) is already a flat list of
`WhenClause{ Patterns: [expr Bool], Body }` + `ElseBody`: a control structure,
no more sugary than `If` or `While`. Lowering it to an `If` chain would need
types (exhaustiveness) *and* would destroy the only information that lets a
backend emit a jump table. Cost of keeping it: ~15 lines, the same ~15 an `If`
chain costs anyway.

`HashLit`'s own version of this section is gone: that migration landed too —
its rewrite (`LowerHashLits`, `LiteralLowering.cpp`) is the same shape as
`ArrayLit`'s, using the stdlib's existing `Hash#[]=` (no stdlib change
needed, unlike `Array<T>#<<`, which had to be added), synthesizing
`Call( Member( tmp, "[]=" ), [ key, value ] )` per entry — the exact node
shape `IndexLowering` already turns a hand-written `tmp[k] = v` into. The
backend lost its `HashLit` dispatch arm and `FailAggregateLiteral` entirely.

## The operator contract (the one that is easy to get wrong)

`Binary` and `Unary` are core nodes, but they mean **two different things**, and
the discriminator is the receiver's `LayoutKind` — never a type name:

- **`Primitive` / `Pointer` layout** → a machine instruction, selected from the
  opaque `Primitive{ Spelling, Bits }` (`"i32"` → `add`, `"f64"` → `fadd`). The
  compiler never learns that `"f64"` means `Float64`; see `rules/zero-hardcode.md`.
- **any other layout** (`String + String`, a user `struct` that
  `include Arithmetic`) → a **method call**, resolved exactly like a `Call`.

**This is realised with no pass and no node.** `MemberType`
(`Sema/.../MemberResolver.cpp`) resolves the operator by its spelling and
records the result in `Context.CalleeResolution[Id.Value]` for `Binary` and
`Unary` just as it does for `Member`. A backend therefore reads:

```
CalleeResolution has Id, Decl != nullptr  ->  emit a call to that method
otherwise (primitive/pointer layout)      ->  emit the instruction for Spelling
```

The recording lives **inside `MemberType`**, not at its call sites, and every
member-ish node (`Member`, `InstanceVar`, `Binary`, `Unary`) goes through that
one function. Resolving an operator and then dropping the resolution — which is
what the code did before — silently pushes member lookup into all three
backends. If you add another node that resolves a member, route it through
`MemberType` too.

The rejected alternative was a post-`TypeChecker` lowering pass turning
non-primitive operators into `Call` nodes. It would have to hand-annotate the
`UnitTypes` of every `Call` it creates. Zero nodes beats that.

## "Every value expression has a type" — the exact wording

`AstInvariant` (`Sema/Private/Passes/AstInvariant.cpp`) checks the second
contract on the set of expressions a backend will actually ask a value at:
`Init` of a `LocalDecl`, `Value` of an `Assign` / `Return`, `Args` and
`BlockArg` of a `Call`, the operands of `Binary` / `Unary` / `Ternary` /
`Deref`, and the elements of an `ArrayLit` / `HashLit`. A `Member` in callee
position or an `Identifier` naming a type is deliberately outside it.

Two exclusions, both recorded by the compiler rather than guessed at:

- **Metadata.** `InferExpr` short-circuits anything `MetadataExprs` marked —
  the arguments of an `@[...]` annotation and of a `macro` invocation. Those
  nodes are spellings consumed at compile time, never evaluated.
- **Generic definition bodies.** Inside `Array<T>`, `Enumerable<T>` or
  `map<U>`, a value of type `T` has no `SemaTypeId` and cannot have one:
  `T` becomes a type at instantiation, and instantiation is monomorphisation,
  which is codegen. `TypeChecker` marks every expression it walks under a
  generic declaration through `UnitTypes::MarkDeferred`, so "deferred until
  instantiation" is distinguishable from "the middle-end forgot". **The
  contract a backend gets is therefore: typed outright in concrete code,
  typed after substitution inside a generic definition.**

## Two receivers that are not values

- **A `module` is a namespace.** It has no nominal, no layout, no `self`; its
  methods are bound as *free functions* (`Layout/TypeBinder.cpp`). Only the
  name is kept, in `TypeStore::DeclareModule`, and it exists for one purpose:
  `MathUtils.square( 4 )` resolves through `LookupFreeFunction` and records a
  `CalleeResolution` like any other call, while `unknown_thing.square( 4 )`
  stays a genuine unknown. A backend sees an ordinary `Call`; the `Member`'s
  object carries no type and must never be evaluated.
- **`*p` is a `Deref`, and its type is the pointee.** "Pointer" is not a name
  the compiler knows: the pointer nominal is whichever stdlib type claims the
  `PointerType` node kind (`@[Literal( PointerType )]`), the same mechanism
  that identifies `Nil` through `NilLiteral`. The pointee is that instance's
  first generic argument. Dereferencing anything else is an error, not a
  silence.

## The structural invariant behind all of this

> **No pass runs after `TypeChecker` except `Analysis` passes that create no
> node.** (`Sema/PassList.inl`)

That is the whole reason "every node a backend sees has a type" is checkable
rather than hoped for. Any future proposal for a "post-typing lowering pass"
must first explain how it types what it creates.

## Known non-goals, refused loudly

These are refused by an explicit diagnostic, never accepted in silence — a flat
refusal is not debt, a silence would be:

- **`T?` / `NilableType`** — needs a sum-type model Volt does not have.
- **`Array#to_string` / `Hash#to_string`** — need bounds on generic parameters
  (`T : Stringable`), which Volt does not have. `"#{ arr }"` reports
  `type Array has no member 'to_string'`.
- **`Symbol#to_string`** — the *name* behind a symbol needs a runtime interner
  table, which is backend work.
- **Un-annotated lambda parameters with no expected type** — needs a
  bidirectional solver; a separate project, with no impact on the backends.
  `samples/Tests/Functional/{Lambda,PointFree}.vl` are the fixtures (the old
  `samples/Functional/FunctionalSpec.vl` path no longer exists): `add_five =
  ( &.+ 5 )` then `add_five( 10 )`. The point-free form is kept in a comment
  right above its typed replacement, not deleted — the samples now compile
  with `add_five = ( x : Int32 ) => x + 5`, but the case they document (a
  `Section` with no expected type has no inferable parameter type) still
  applies to the commented-out original.
- **Heterogeneous literals / tuples.** `[ "Alice", "a@b.c", 42 ]` needs a sum
  type or a `Tuple<…>`, the same wall `T?` hits. `samples/Tests/ControlFlow/
  ForLoop.vl`'s `for_array` is rewritten to a homogeneous `Array<User>` with
  the original heterogeneous array literal kept in a comment above it.
- **`break <value>` outside a block.** Giving `break x` a value that becomes
  the enclosing call's result needs the result type of the call that owns the
  block to be the join of every `break`'s value in the body; `Frontend::Break`
  is walked as a leaf by `DeclStmtWalker.cpp:250` and nothing joins anything.
  Refused by name in the backend (see the `break` phase in
  `.agents/PLAN_LLVM.md`). `samples/Tests/ControlFlow/BreakNext.vl`'s
  `test_break_with_value` is rewritten to an accumulator (`found = x` before
  `break`) that exercises the same non-local exit without depending on
  `break`'s value.
- **Integer literal suffixes are parsed and then ignored by Sema.** `0_u64`
  types as `Int32`, because `LiteralType` inserts every `IntLiteral` into
  `UnconstrainedLiterals` without ever reading the suffix. A real missing
  feature, not a regression, and its own piece of work.
- **A method with no `-> T` has no return type**, so its value in an
  expression is untyped. There is no return-type inference; the stdlib
  annotates everywhere and samples must too.
- **Several `samples/Syntax/**` fixtures do not `check`** — they call
  `Array#length`, `puts`, `Hash#each`, `to_json`, which the stdlib does not
  declare. Those are stdlib gaps; they are parse fixtures, covered by `Golden`
  tests only. `AstInvariant`'s typing half is enforced on `source/Lib/**` and
  `samples/Sema/**`; its residual-sugar half on everything
  (`tests/meson.build`).
- **The JSX runtime is not declared.** `JsxLowering` is complete — zero Jsx
  nodes survive — but it emits `Volt.JSX.create_element( tag, props, children )`
  and no stdlib type declares it, so `.vlx` files lower correctly and then type
  as nothing. Declaring it needs an element type and heterogeneous `children`,
  i.e. a sum type, the same wall `T?` hits. It is silent rather than loud
  because a `Member` on a receiver that has no type says nothing by design;
  making *that* loud was tried and produced 253 false positives across the
  corpus (bare names legitimately resolve later or elsewhere), so the honest
  record is this line, not a wrong diagnostic.
