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

## graphify

This project has a graphify knowledge graph at graphify-out/.

Rules:
- Before answering architecture or codebase questions, read graphify-out/GRAPH_REPORT.md for god nodes and community structure
- If graphify-out/wiki/index.md exists, navigate it instead of reading raw files
- After modifying code files in this session, run `graphify update .` to keep the graph current (AST-only, no API cost)

## Two reflexes on every change

1. **Format & build.** Run `volt-build format` then `volt-build` before you call
   a task done (Allman style, `SpacesInParens`, column 170) — format is parallel and
   per-file cached, so it only touches what changed. The build is `-Werror`; warnings are failures.
   All build, format, and test tasks must go through `volt-build`.

   **Never run `volt-build tidy`, and ignore clangd.** Both are clang front-ends,
   and clang does not implement C++26 static reflection (P2996) yet, so it cannot
   parse `Meta/Reflect.hpp` — nor most of `Frontend/`, `Sema/` and `Driver/` behind
   it. Everything they report on those files is a bogus artefact of a failed parse
   (`No type named 'ExprList'`, `no type named '__index_type'`, …). GCC via
   `volt-build` is the only ground truth. See
   [`rules/cpp-style.md`](rules/cpp-style.md).
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
- **Unreal C++ style, C++26, `Public/Private`, `-Werror`.** See
  [`rules/cpp-style.md`](rules/cpp-style.md).
- **The `volt` CLI surface is a contract.** Subcommands (`run`, `repl`, `parse`,
  `check`, `version`, `help`, `circuit`, then `build`, `format`) and their
  options are specified once in [`rules/cli-surface.md`](rules/cli-surface.md);
  `Main.cpp` stays a thin command table over `Driver`.
- **Symbols crossing a `.so` boundary must be exported.** Modules build with
  `-fvisibility=hidden`; anything a *different* module calls needs the
  generated `<MODULE>_EXPORT` macro, or `VOLT_BUILD_SHARED=ON` link-fails late
  with a wall of mold `undefined symbol` errors. See
  [`rules/shared-lib-exports.md`](rules/shared-lib-exports.md).
