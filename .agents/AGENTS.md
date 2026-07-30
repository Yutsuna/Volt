# `.agents/` — How to Contribute to Volt (Human or AI)

This directory is the **portable, versioned source of truth** for the agents, rules, and skills that frame every contribution to the Volt compiler. Claude Code reads subagents from `.claude/agents/` and skills from `.claude/skills/`; those are symlinks back here, so editing a file under `.agents/` updates the harness harness.

```
.agents/
  rules/    always-on constraints every change must satisfy
  agents/   specialised subagents (AST, parser, sema, build, review)
  skills/   step-by-step recipes for common, repeatable edits
```

## Graphify Knowledge Graph

This project maintains a Graphify knowledge graph located at `graphify-out/`.

### Mandatory Rules:
- Before answering architecture or codebase questions, read `graphify-out/GRAPH_REPORT.md` for god nodes and community structure.
- If `graphify-out/wiki/index.md` exists, navigate it instead of reading raw files.
- After modifying code files in a session, run `graphify update .` to keep the graph current (AST-only, no API cost).

## Two Reflexes on Every Change

1. **Build & Format, through the IDE.** Build and test through the IDE (CLion) — never "run" a module configuration: a module (`Core`, `Frontend`, `Sema`, `Driver`, `BackendLLVM`, …) is a library target (`.so`/`.a`), not an executable. Run an executable or test configuration instead (e.g. `All CTest`), which builds every dependency first. The build is `-Werror`; warnings are treated as failures. Run the `format` configuration (Allman style, `SpacesInParens`, column 170) once, at the **end of a phase** — not mid-phase, since it reformats every touched file in one pass and re-running it early wastes time. Run the `tidy` configuration only at the **end of an epic** (once every phase is complete): it is expensive and, under C++26, occasionally flags things that are not actually a problem — prefer the IDE's own diagnostics while iterating. See [`rules/cpp-style.md`](rules/cpp-style.md).
2. **Keep the Map Current.** After a significant architecture change, run `graphify update .` (AST-only, no API cost) so `graphify-out/` reflects the modified code. See [`rules/graphify.md`](rules/graphify.md).

## The Meta-First Bet

Volt's foundational principle is that **adding a feature should take ~10 lines, not 500**. Before writing new boilerplate, check whether the change is simply *one line in a manifest* (`AST/Nodes.inl`, `Lexer/TokenKind.inl`, `Parser/Pratt.inl`, `Sema/PassList.inl`) plus a small struct. If you find yourself writing a manual visitor per node or a `switch` over kinds, stop — that is what `Reflect` + `Overloaded` are designed for. See [`rules/meta-first.md`](rules/meta-first.md).

## Non-Negotiables

- **Zero Hardcoding of Volt Types.** The C++ compiler never hardcodes names like `Int`, `String`, or `Array`. It operates strictly on Memory Layouts; type names reside in the Volt standard library (`source/Lib/`) alongside annotations. See [`rules/zero-hardcode.md`](rules/zero-hardcode.md).
- **Value AST.** Arena storage with typed `Id`s; smart pointers are strictly forbidden in the AST. See [`rules/ast-value.md`](rules/ast-value.md).
- **Arena-by-Index Pass Sweeping.** A rewriting pass sweeps the arena by index. It copies the source node by value and assigns the slot; it must never hold an arena reference across an `Add()` invocation — doing so silently loses rewrites due to vector reallocation. See [`rules/ast-rewrite.md`](rules/ast-rewrite.md).
- **27-Node Core AST Contract.** No sugar survives `Lowering`, every value expression is typed (outright in concrete code, or after substitution inside a generic body), and `AstInvariant` (Order 40) enforces both on every build. What is refused loudly upstream rather than delegated to a backend is specified in [`rules/core-ast.md`](rules/core-ast.md).
- **Unreal C++ Style, C++26, `Public/Private`, `-Werror`.** See [`rules/cpp-style.md`](rules/cpp-style.md).
- **The `volt` CLI Surface Contract.** Subcommands (`run`, `repl`, `parse`, `check`, `version`, `help`, `circuit`, `build`, `format`) and their options are specified once in [`rules/cli-surface.md`](rules/cli-surface.md); `Main.cpp` remains a thin command table routing to `Driver`.
- **Export Symbols Across `.so` Boundaries.** Modules compile with `-fvisibility=hidden`. Anything invoked by a *different* shared module requires the generated `<MODULE>_EXPORT` macro; otherwise `VOLT_BUILD_SHARED=ON` link builds will fail with mold `undefined symbol` errors. See [`rules/shared-lib-exports.md`](rules/shared-lib-exports.md).
- **Dual-Mode Build Performance Strategy.** Non-unity builds by default for instant 1s incremental local edits; `unity` flag enabled for clean CI/Release builds. See [`rules/build-performance.md`](rules/build-performance.md).
