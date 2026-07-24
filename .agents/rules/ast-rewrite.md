# Rule: AST rewrite — sweep the arena by index, never hold a reference across `Add()`

A rewriting pass (`*Lowering`) turns one node kind into another **in place**. The
only safe shape is:

```cpp
// 1. bound the sweep BEFORE creating anything
const std::size_t OriginalCount = Context.ExprCount();

for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
{
    const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
    if ( KindOf( Context.Expr( Id ) ) == Frontend::ExprKind::Target )
    {
        // 2. the helper copies the source node out, then Add()s freely
        // 3. the assignment sequences the call before the destination
        Context.Expr( Id ) = LowerTarget( Id );
    }
}
```

Three properties, all load-bearing:

- **Copy out first.** `LowerTarget` starts with
  `const Frontend::Target Node = std::get<Frontend::Target>( Context.Expr( Id ) );`
  — a value, not a reference. Every `Add()` after that is harmless.
- **Return a node, assign the slot.** `Context.Expr( Id ) = f( Id )` sequences
  the call before the evaluation of the left operand (C++17), so an `Add()`
  inside `f` cannot invalidate the destination. Parents refer to the `Id`, so
  the whole tree updates at once and traversal order is irrelevant
  (`rules/ast-value.md`).
- **Bound the loop before the first `Add()`.** Nodes a lowering appends land
  past `OriginalCount`; they are never of the kind being lowered, so skipping
  them is deliberate. Sub-expressions are parsed first, hence carry smaller
  indices, so `0 → OriginalCount` desugars innermost-first for free — which is
  what nested sugar (`(&.trim) >> (&.prefix("_"))`, `5 |> f |> g`) needs.

## The counter-example, measured

`Core::Arena` stores in a `std::vector`
(`Core/Support/Arena.hpp`, `push_back` / `emplace_back`): **any `Add()` may
reallocate and invalidates every reference obtained before it.**

`FunctionalLowering` used to drive its rewrite from a reflective walk —
`WalkExpr( Frontend::ExprId &Id )` receiving a reference *bound inside the live
parent node* via `Meta::ForEachField`, then `Id = LowerSection( Id )` where
`LowerSection` performs six `Add()`. The write landed in freed memory. The
symptom is not a crash but a **silently lost rewrite, dependent on file size** —
the same program preceded by *N* unrelated `def`s:

```
pad=0..3 → 1 residual Section     pad=4..7 → 2 residual Sections
```

after the fix, 0 for every padding. Regression samples:
`samples/Functional/ArenaStability.vl` and `ArenaStabilityPadded.vl`.

A reflective walk is also a duplicate of `Meta::ForEachField` that
`rules/meta-first.md` forbids: the index sweep needs no traversal at all, since
every node lives flat in the arena. Converting `FunctionalLowering` removed
~120 lines.

## Per-category arenas: a safety you must not lean on

Each category has its own arena, so a `StmtNode &` is invalidated only by a
`Stmt` `Add()`, not by an `Expr` one. `CaseLowering` relies on exactly that: it
holds `Frontend::WhenClause &Clause` (from the Stmt arena) across several
`Context.Add( ExprNode )`. It is correct today, and it becomes UB the day
someone adds a `Stmt` inside that loop. Prefer copy-out / write-back even when
the categories differ; if you keep a cross-category reference, say in a comment
which arena must stay untouched.

## Conforming passes (reference implementations)

`JsxLowering` (copies the node out, comments the hazard), `PipelineLowering`,
`CaseLowering`, `EnumLowering`, `FunctionalLowering`. `MacroExpansion` uses
copy-out / write-back on the *Decl* arena (`ExpandIn`). `MagicExpansion` creates
no node at all.

## Checklist before you call a lowering done

1. No `ExprId &` / `StmtId &` / `DeclId &` parameter anywhere in the pass.
2. No `Meta::ForEachField` used to *write* — reflection is for reading.
3. The census is 0 for the lowered kind, **on two files differing only by
   padding**:
   ```sh
   ./build/bin/Volt_d parse --lowered --no-color --no-location F | grep -cE '─ Target\b'
   ```
4. `volt-build debug asan` on a sample exercising the pass: no report.
