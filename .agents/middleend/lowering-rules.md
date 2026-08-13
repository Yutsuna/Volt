# MiddleEnd Specification: The Rewrite Rule Engine (`LoweringRules`)

`MiddleEnd/Lowering/LoweringRules.{hpp,cpp}` is the single sweep every syntactic
lowering pass is written against.

---

## The problem it removes

A lowering pass is always the same program: sweep the `Expr` arena by index, find
the nodes of one kind, replace each with the core-AST shape it desugars to. Every
pass in the module used to spell that out again — its own `class XRewriter`, its
own `for` over `ExprCount()`, its own `KindOf` test, its own copy-out/write-back.

The sweep is the part that is easy to get subtly wrong (`rules/ast-rewrite.md`:
never hold a reference across `Add()`, never let the loop see nodes it just
appended), and it was duplicated once per pass.

Here it is written once. A pass supplies a **rule**; the bound, the ordering, the
write-back and the counter belong to `RuleRegistry::Run`.

---

## `CRewriteRule`

```cpp
template <typename T>
concept CRewriteRule = requires ( T Rule, RewriteContext &Ctx, Frontend::ExprId Id ) {
    { T::Kind() }           -> std::same_as<Frontend::ExprKind>;
    { Rule.Match( Ctx, Id ) } -> std::same_as<bool>;
    { Rule.Apply( Ctx, Id ) } -> std::same_as<Frontend::ExprNode>;
};
```

| Member | Meaning |
|---|---|
| `Kind()` | the `ExprKind` this rule claims. The registry buckets on it, so `Match` is never called on a node of any other kind. |
| `Match()` | is *this* node this rule's business? It answers the residual question — *"is the RHS already a Call?"* — never *"is this a Pipeline?"*. A rule claiming every node of its kind returns `true` and costs nothing. |
| `Apply()` | the node it becomes. Returned **by value**; the registry writes it back, which keeps the copy-out/write-back discipline out of every individual rule. |

Rules are stateless by construction: the registry default-constructs one per
call, so a rule cannot accumulate anything across the nodes it visits.

### `RewriteContext`

```cpp
struct RewriteContext
{
    Frontend::AstContext        &Ast;
    Volt::Core::DiagEngine::Bag &Diags;
};
```

Deliberately smaller than `Core::PassContext`. A syntactic rewrite runs before
any type exists, so a rule that could reach `Types`/`Values` would be a rule that
could *depend* on them — and that is precisely the property that lets these
passes run at orders 8–26 at all.

---

## `RuleRegistry`

```cpp
Registry.Add<PipeIntoCall>( "PipeIntoCall", 10 )
        .Add<PipeIntoValue>( "PipeIntoValue", 0 );
```

**Bucketed dispatch.** Rules are stored in `std::array<std::vector<RewriteRule>,
KindCount>`, indexed by the `ExprKind` each claims. `KindCount` is
`std::variant_size_v<Frontend::ExprNode>` — monostate plus one per `VOLT_EXPR`
row — so a new node kind widens the array with no edit. Adding a rule costs
nothing to the passes that do not claim that kind, which is what keeps *"one more
rule"* from meaning *"one more test on every node in the file"*.

**Priority.** Orders rules *within* a bucket, highest first; the first whose
`Match` accepts wins. Equal priorities keep registration order (the insert point
is found rather than the bucket re-sorted). This is the mechanism that replaces a
branch inside one rule's `Apply` with two sibling rules.

**The sweep.** One bounded index pass:

- the bound is read **once, before** the first rewrite, so nodes a rule appends
  are never themselves visited — a rule's output is core AST by definition, and
  re-examining it is how a rewrite engine loops forever;
- ascending order lowers the innermost node of a nest first (`5 |> add |> double`),
  because sub-expressions parse first and so hold smaller indices — exactly what
  the hand-written sweeps relied on;
- at most one rule fires per node per sweep. A rewrite needing a second rule over
  its own output wants a second pass, not a re-entrant sweep.

Rules are type-erased to a pair of function pointers, so rules of unrelated types
share a bucket with no allocation and no indirection beyond the call.

---

## The two converted passes

### `PipelineLowering` (order 9) — priority in action

`a |> f` becomes `f( a )`, and what it does depends entirely on whether the
right-hand side is already a call. That is a question about *which rule applies*,
not a step inside one, so it is two rules rather than one with a branch:

| Rule | Priority | Match | Apply |
|---|---|---|---|
| `PipeIntoCall` | 10 | RHS is a `Call` | prepend the piped value to the existing arguments |
| `PipeIntoValue` | 0 | always | RHS is the callee; the piped value is the whole argument list |

Each is a single unconditional transformation. `5 |> scale( 3 )` and `5 |> double`
are two rules, not two branches.

### `DotCallLowering` (order 23) — the honest catch-all

Every `DotCall` reaching order 23 is an implicit-self call, so `DotCallToSelf`
claims all of them: `Match` returns `true`, which is the honest spelling of
*"there is no residual question here"*. `Apply` builds `self.method( args )`,
copying the node out first because both its `Add()`s can move the arena.

Both passes keep their `PassList.inl` entries and their `PassStats` counters
unchanged; only the sweep is now shared.
