# Rule: C++ style

- **C++26**, GNU/Clang toolchain, built under `-Werror`. A warning is a failure.
- **Unreal-flavoured PascalCase**: types, functions and locals are `PascalCase`
  (`ParseExpr`, `TypeStore`, `const std::size_t Count`); booleans read `bAbstract`,
  `bComponent`. Member layout follows the existing nodes (public API first,
  private state last).
- **`Public/Private` split** per module: headers under `Public/Volt/<Module>/…`,
  translation units under `Private/…`. A module's public include root is its
  `Public/` directory (`target_include_directories(... PUBLIC Public)`).
- **Modules** are declared via `library(...)` in `source/Volt/<Module>/meson.build`
  and registered in `source/Volt/meson.build` (`subdir('<Module>')`). Libraries are shared and
  cascade their `DEPS` (`Frontend → Core`, `Sema → Frontend`, `Driver → Sema`).
- **Formatting is mechanical**: run the `format` IDE configuration (Allman braces,
  `SpacesInParens`, `ColumnLimit: 170` — parallel, per-file cached). Do not
  hand-format; let the tool do it. Run it once, at the end of a phase — not
  after every edit.
- **`[[nodiscard]]`** on every pure accessor / factory. Prefer free functions +
  `std::visit(Overloaded{…})` over virtual dispatch.

Before finishing: build and test through the IDE (see skill
`format-and-check`) — never "run" a module target, only an executable or test
configuration; a clean, warning-free build under `-Werror` and a green test
run are both required.

## Static analysis & Tidy

The `tidy` IDE configuration runs static analysis over the codebase. It is
expensive and its C++26 diagnostics are sometimes not pertinent — run it only
once, at the end of an epic (every phase complete), never mid-phase. Prefer
the IDE's own inspections (clangd / IntelliJ diagnostics) while iterating.
Ensure formatting (`format`), a clean `-Werror` build, and tests all pass
before calling an epic done.
