# Rule: meta-first — a feature is ~10 lines

Volt is built so that most changes are **one line in a manifest + a small
struct**, never a 500-line traversal. The manifests are the single source of
truth; everything generic (enums, variants, dispatch tables, name tables,
printers, walks) is *derived*.

The manifests:

| Manifest                          | Drives                                             |
|-----------------------------------|----------------------------------------------------|
| `Frontend/AST/Nodes.inl`          | every `*Kind` enum, `*Node` variant, node-name LUT |
| `Frontend/Lexer/TokenKind.inl`    | token enum, spelling table, keyword lookup         |
| `Frontend/Parser/Pratt.inl`       | infix binding-power / parselet table               |
| `Sema/PassList.inl`               | ordered pass registry                              |

Before writing code, ask: *is this a new manifest entry?*

- New AST node → 1 line in `Nodes.inl` + a struct with `VOLT_FIELDS(...)`. No
  visitor: `ForEachField` / `AstPrinter` pick it up for free. See skill
  `add-ast-node`.
- New token → 1 line in `TokenKind.inl`.
- New sema pass → 1 line in `PassList.inl` + one function. See skill `add-sema-pass`.

If a change makes you write a `switch` over every kind, or a near-duplicate of an
existing traversal, you are fighting the architecture. Reach for
`Meta/Reflect.hpp` (`ForEachField`) and `Meta/Overloaded.hpp` instead — a pass is
`std::visit(Overloaded{ [&](Target&){…}, [&](auto&){ WalkFields(…); } }, Node)`.

## `VOLT_EXPR_SUGAR` — a second macro, still one line per node

`Nodes.inl` declares expressions with two macros, not one. `VOLT_EXPR( Name )`
is a core node; `VOLT_EXPR_SUGAR( Name )` is a node a `Lowering` pass must have
rewritten before `TypeChecker` runs (`rules/core-ast.md`). The manifest defines

```cpp
#ifndef VOLT_EXPR_SUGAR
    #define VOLT_EXPR_SUGAR( Name ) VOLT_EXPR( Name )
#endif
```

so **every existing consumer keeps seeing all 36 nodes** — the enum in
`Expr.hpp`, the variant, the name LUT, `ForEachField`, the printer. Only a
consumer that defines `VOLT_EXPR_SUGAR` itself tells the two apart, and there
are exactly two:

- `Sema/Private/Passes/AstInvariant.cpp` builds a `constexpr std::array` of the
  sugar kinds and reports any survivor. **No `switch`** — membership on a
  generated set.
- `tests/AstInvariant.cmake` reads the same lines out of the manifest with a
  regex, so the corpus-wide census cannot drift from the compiler's own.

Marking a new node as sugar is therefore **one word** in `Nodes.inl`, and
forgetting to lower it is a build failure rather than something a backend
author discovers. The same shape applies to `PassStats`: `Merge` and
`check --metrics` both walk it with `Meta::ForEachField`, so a new counter is
one field and reaches the CLI with no other edit.
