# MiddleEnd Specification: Compile-Time Introspection (`TraitEngine`, `typeof`)

Volt answers a small, closed set of questions about types **while compiling**, and
replaces the asking expression with its answer. Nothing survives to run time: a
program that asks `user.is_a? Admin` contains a `BoolLiteral` by the time any
backend sees it, and no symbol named `is_a?` exists anywhere in the binary.

Two constructs live here, and they differ in what they *yield*:

| Construct | Operand | Yields | Where it is answered |
|---|---|---|---|
| Receiver traits (`is_a?`, `includes?`, …) | a type name or a symbol | a `BoolLiteral` | `Analysis::FoldReceiverTrait`, during inference |
| `typeof( expr )` | an expression | a **type** | `TypeSystem::ResolveTypeExpr`, via a sink hook |

Both are compiler vocabulary, reserved in `Frontend/Lexer/TokenKind.inl`, for the
same reason `trivially_destructible?` already is: the compiler answers them, so a
stdlib type declaring its own `is_a?` would be silently shadowed rather than
called. `rules/zero-hardcode.md` is respected throughout — every answer is read
off the `TypeStore`'s own structure (`Includes`, `Super`, `Members`), never off a
Volt type name.

---

## 1. Receiver traits

### Surface

Five spellings, declared once as `VOLT_TRAIT_KEYWORD` rows in `TokenKind.inl`:

```volt
user.includes? Greetable          # a mixin the type includes, transitively
dog.inherits_from? Animal         # strictly above: never reflexive
dog.is_a? Animal                  # identity, ancestry, or inclusion
row.has_field? :email             # a data member, own body or inherited
row.has_method? :save             # a method, own body or inherited
```

Both spellings mean the same thing — `dog.is_a? Animal` and `dog.is_a?( Animal )`
— and a **type name** answers the same questions its instances do
(`Dog.includes? Greetable`).

### Why they are reserved words, not method names

The parser only absorbs a paren-less argument after `.` for these five tokens.
`a.b c` still parses as a member access followed by a separate statement, so no
existing program moves. Making them keywords is what buys that narrow exception
without a grammar-wide ambiguity — see `ParsePostfix`'s `Dot` arm
(`Frontend/Private/Parser/ParseExpr.cpp`).

The operand is parsed at binding power 65, above every infix operator that can
legally follow the question, so `x.is_a? T and y` groups as `( x.is_a? T ) and y`.

### The engine

`MiddleEnd/ConstEval/TraitEngine.{hpp,cpp}` plus the `TraitOps.inl` manifest.

Adding a trait is **one row in `TokenKind.inl`, one row in `TraitOps.inl`, and one
line in `TraitEngine.cpp`** — never a branch at the interception seam, and never
anything in a backend. A `static_assert` pins the two manifests to the same
length, so a keyword row with no meaning row is a build error rather than a
silently-wrong program.

Every evaluator is one expression over three shared traversals:

| Traversal | Answers |
|---|---|
| `IncludesMixin` | own `Includes`, mixins of mixins, and everything inherited |
| `InheritsFrom` | `TypeSystem::IsSubclassOf` minus the reflexive case |
| `HasMemberOfKind` | `TypeStore::LookupMember` plus an `EMemberKind` test |

All are depth-bounded at 16, the same bound and the same reason as
`TypeStore::LookupMember`: a malformed cyclic hierarchy must not hang sema.

### The interception seam

`Analysis::FoldReceiverTrait`, called at the **top of `CallType`**
(`Analysis/Private/ExprInferencer.cpp`) — before the callee is inferred.

That ordering is the whole design. No type declares `is_a?`, so letting the
`Member` callee infer first would report *"type Dog has no member 'is_a?'"* before
the trait could ever be answered. Only the **receiver** is inferred; the trait's
own `Member` node is never typed and never reaches `CalleeResolution`. That is
what makes "no call is emitted" structural rather than a promise — there is no
`Resolution` for a backend to emit a call from.

The fold then writes a `BoolLiteral` into the node's slot and publishes its type.

> A `SymbolLiteral`'s lexeme is interned from the `:` onward (the lexer makes the
> token at the colon), and a member is named without one — so `has_field? :x`
> strips the leading colon before looking the name up.

### Inside a generic body

`@value.is_a? Tagged` inside `Box<T>` cannot be answered while `T` is still a
parameter. Three things happen, in this order:

1. The definition-body walk finds an invalid receiver nominal, **leaves the node
   standing**, types it as a truth value, and marks the *argument* deferred
   (`UnitTypes::MarkDeferred`) — without which `AstInvariant` would report it as
   an expression nobody typed. Nothing descends into a trait's operand, so it
   would otherwise never acquire a type.
2. `TypeSystem::ReinstantiateBody` walks the same shared body once per
   instantiation with the arguments fixed. The receiver is concrete there, so the
   trait folds.
3. Because `TypeCheckerContext::Redirects` is set on that walk, the answer is
   recorded in the redirect map against a **new** node rather than overwriting
   the body both instantiations share — the same contract `ClosureLifting`
   already keeps for a generic body's closure literals.

The folded node's type is published on whichever node the backend will actually
read: the target in mutate mode, the new node in redirect mode. Skipping the
latter is how an instantiated body reaches codegen holding a literal nobody
typed — it fails as *"the type claiming BoolLiteral has no integer layout"*.

An un-instantiated generic body is never emitted, so no backend can meet an
unfolded trait.

---

## 2. `typeof( expr )`

### Surface

`typeof` names the type an expression *has*, written wherever a type is written:

```volt
count : Int32 = 41
mirrored : typeof( count ) = 1              # a local annotation
doubled  : typeof( count * 2 ) = 84         # over an expression, not just a name
numbers  = Array<typeof( count )>.new       # a generic argument
width    = sizeof( typeof( count ) )        # composed with the other type question
```

Always parenthesised: the operand is an expression, so without a closing
delimiter `typeof x * 2` has no reading the grammar can pick.

It is **not** a `TypeTrait`. `TypeTrait` takes a type and yields a constant;
`typeof` takes a value and yields a type, so it lives in the *type* grammar
(`VOLT_TYPE( TypeOfType )`), not the expression grammar.

### Resolution — the one node built from a value

`ResolveTypeExpr` resolves every other type shape through a single reflected
line: the node's name is looked up in the store's node-kind table and its
`TypeId` fields become the arguments. `TypeOfType` cannot go that way — it has no
`TypeId` field to recurse on and claims no node kind — so it is the one explicit
branch besides `TypeRef`.

Answering it means *inferring an expression*, and inference lives in `Analysis`,
above `TypeSystem` in the module DAG. Rather than invert that dependency, the
sink carries a hook:

```cpp
SemaTypeId ( *InferHook )( void *State, Frontend::ExprId Id ) = nullptr;
void *InferState                                             = nullptr;
```

`TypeCheckerContext::MakeSink()` installs the walk itself. The hook is captureless
so it decays to a plain function pointer — which is what lets `TypeSystem` carry
it **without any header there ever naming `Analysis`**. The `#include` graph stays
acyclic; only the call is inverted.

`MakeSink()` is also the single place the four fields every body-annotation sink
repeated now live. Building a `UnitSink` by hand elsewhere is not wrong, only
weaker: a `typeof( x )` resolved through it answers nothing.

### Where it is refused

| Sink | Position | Behaviour |
|---|---|---|
| `UnitSink` **with** hook | any annotation inside a body | resolves |
| `UnitSink` **without** hook | `ReinstantiateBody`'s best-effort resolve | invalid id, no diagnostic — reporting would fire once per instantiation |
| `SigSink` | a declaration's signature (field, parameter, return) | **hard error** |

A signature is published for *other units* to resolve against, and an
expression's type is a fact about one body in one unit whose AST that other unit
never sees. So it is refused out loud:

```
error: 'typeof' cannot be used in a declaration's type — it names the type of an
       expression, which only exists inside a body
```

The alternative — an invalid id and silence — is a field whose type is nothing.

---

## Tests

`samples/Tests/Traits/` — runtime samples, the strongest available proof.

- `CompileTimeTraits.vl` pins all five traits across identity, ancestry,
  inclusion and member lookup, both spellings, the static form, operator
  precedence, and the two-instantiation generic case.
- `TypeOf.vl` pins local annotations, expression operands, generic-argument
  position and composition with `sizeof`.

That these files **link at all** is the compile-time half of the test: nothing in
`source/Lib/` declares any of the five names, so a trait that survived as an
ordinary call would have no symbol to call. `nm` on the built binary reports zero
matches for any trait spelling.

`volt parse --lowered` deliberately shows the traits *unfolded* — that mode runs
only `EPassKind::Lowering` passes, and the fold happens during `TypeChecker`
(order 30). The `.lowered.golden` files therefore lock in the parse shape, not the
answer.
