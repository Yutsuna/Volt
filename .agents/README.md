# `.agents/` — how to contribute to Volt (human or AI)

This directory is the **portable, versioned source of truth** for the agents,
rules and skills that frame every contribution to the Volt compiler. Claude Code
reads subagents from `.claude/agents/` and skills from `.claude/skills/`; those
are symlinks back here, so editing a file under `.agents/` updates the harness.

```
.agents/
  rules/    always-on constraints every change must satisfy
  agents/   specialised subagents (AST, parser, sema, build, review)
  skills/   step-by-step recipes for common, repeatable edits
```

## Two reflexes on every change

1. **Format & lint.** Run `clang-format -i` on every file you touch (repo
   `.clang-format`: LLVM/Allman, `SpacesInParens`, column 170) and respect
   `.clang-tidy` before you call a task done. The build is `-Werror`; warnings
   are failures.
2. **Keep the map current.** After a significant architecture change, run
   `graphify update .` (AST-only, no API cost) so `graphify-out/` still reflects
   the code. See [`rules/graphify.md`](rules/graphify.md).

## The meta-first bet

Volt's whole point is that **adding a feature is ~10 lines, not 500**. Before
writing new machinery, check whether the change is really *one line in a
manifest* (`AST/Nodes.inl`, `Lexer/TokenKind.inl`, `Parser/Pratt.inl`,
`Sema/PassList.inl`) plus a small struct. If you find yourself writing a visitor
per node or a `switch` over kinds, stop — that is what Reflect + Overloaded are
for. See [`rules/meta-first.md`](rules/meta-first.md).

## Non-negotiables

- **Zero hardcode of Volt types.** The C++ compiler never mentions `Int`,
  `String`, `Array`. It reasons in Memory Layouts; type names live in the Volt
  stdlib (`source/Lib/`) + annotations. See [`rules/zero-hardcode.md`](rules/zero-hardcode.md).
- **Value AST.** Arena + typed `Id`s, never smart pointers in the AST. See
  [`rules/ast-value.md`](rules/ast-value.md).
- **Unreal C++ style, C++23, `Public/Private`, `-Werror`.** See
  [`rules/cpp-style.md`](rules/cpp-style.md).
