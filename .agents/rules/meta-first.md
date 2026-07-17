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
