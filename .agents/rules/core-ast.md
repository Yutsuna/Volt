# Rule: core AST — the contract a backend consumes

A backend is written by declarative pattern-matching over a **core AST**. This
file is that contract: what a backend may be handed, and what it may assume.
`Nodes.inl` declares **36 `VOLT_EXPR`** — **27 core, 9 sugar**.

Two invariants make the contract mechanical rather than aspirational, and
`AstInvariant` (order 40) checks both on every build:

1. **No sugar survives `Lowering`.** The 9 nodes below are gone from the arena
   by the time `TypeChecker` runs.
2. **Every expression in value position has a type.** `Values.Has( Id )` holds
   for it.

## The 9 sugar nodes

`Interp` · `Index` · `DotCall` · `Section` · `Composition` · `Pipeline` ·
`JsxElement` · `JsxFragment` · `JsxText`

They are marked `VOLT_EXPR_SUGAR` in `Frontend/AST/Nodes.inl` — one line each,
and that mark is what `AstInvariant` reads. Adding a sugar node is one line;
forgetting to lower it is a build error, not a discovery made in the backend.

## The 27 core nodes

| Category | Nodes | What a backend does with it |
|---|---|---|
| Terminals (9) | `IntLiteral` `FloatLiteral` `StringLiteral` `CharLiteral` `BoolLiteral` `NilLiteral` `SymbolLiteral` `ArrayLit` `HashLit` | materialise a constant / aggregate from the `MemoryLayout` of the type that claimed the node kind via `@[Literal]` (`NominalType::LiteralSlots`) |
| Access (6) | `Identifier` `InstanceVar` `SelfExpr` `SuperExpr` `Member` `Deref` | load / GEP |
| Operations (3) | `Call` `Assign` `Ternary` | call through `CalleeResolution`, store, select/branch |
| Operators (2) | `Binary` `Unary` | see below |
| Control (3) | `CaseExpr` `BeginExpr` `RaiseExpr` | test chain / jump table; EH |
| Closures (2) | `Lambda` `Block` | function + `ClosureEnvFrame` (size, alignment, `bEscapes` already computed) |
| Inert (2) | `GenericInst` `SizeOf` | carry no runtime value: read `Values.Get( Id )` (resp. the layout size) and **never descend into them** |

### Why `ArrayLit` / `HashLit` / `CaseExpr` are core, not sugar

They are not sugar being tolerated; lowering them would *cost* more than
keeping them.

- `ArrayLit` / `HashLit` are **literals**, exactly like `StringLiteral` (itself
  an aggregate `{ data, size }`). They are already fully typed — `[1,2,3]` is an
  `Array<Int32>` and `arr.push` resolves on it. Lowering them to `Array.new` +
  `push` needs the generic argument, which is only known *after* `TypeChecker`,
  so it needs a pass that hand-annotates the types of what it creates — the one
  thing §9's structural invariant forbids.
- `CaseExpr` after `CaseLowering` (order 22) is already a flat list of
  `WhenClause{ Patterns: [expr Bool], Body }` + `ElseBody`: a control structure,
  no more sugary than `If` or `While`. Lowering it to an `If` chain would need
  types (exhaustiveness) *and* would destroy the only information that lets a
  backend emit a jump table. Cost of keeping it: ~15 lines, the same ~15 an `If`
  chain costs anyway.

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
  `samples/Functional/FunctionalSpec.vl` is the fixture: `add_five = ( &.+ 5 )`
  then `add_five( 10 )`. It is a `Golden` (parse) sample and does not
  type-check, by design.
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
  (`tests/AstInvariant.cmake`).
- **The JSX runtime is not declared.** `JsxLowering` is complete — zero Jsx
  nodes survive — but it emits `Volt.JSX.create_element( tag, props, children )`
  and no stdlib type declares it, so `.vlx` files lower correctly and then type
  as nothing. Declaring it needs an element type and heterogeneous `children`,
  i.e. a sum type, the same wall `T?` hits. It is silent rather than loud
  because a `Member` on a receiver that has no type says nothing by design;
  making *that* loud was tried and produced 253 false positives across the
  corpus (bare names legitimately resolve later or elsewhere), so the honest
  record is this line, not a wrong diagnostic.
