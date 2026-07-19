# Rule: C++ style

- **C++26**, GNU/Clang toolchain, built under `-Werror`. A warning is a failure.
- **Unreal-flavoured PascalCase**: types, functions and locals are `PascalCase`
  (`ParseExpr`, `TypeStore`, `const std::size_t Count`); booleans read `bAbstract`,
  `bComponent`. Member layout follows the existing nodes (public API first,
  private state last).
- **`Public/Private` split** per module: headers under `Public/Volt/<Module>/…`,
  translation units under `Private/…`. A module's public include root is its
  `Public/` directory (`target_include_directories(... PUBLIC Public)`).
- **Modules** are declared with `VoltModule(NAME .. TYPE .. SOURCES "Private/*.cpp"
  PUBLIC_INCLUDES "Public/" DEPS ..)` and registered in
  `cmake/VoltBuild.cmake`'s `VoltAddModules(...)`. Libraries are shared and
  cascade their `DEPS` (`Frontend → Core`, `Sema → Frontend`, `Driver → Sema`).
- **Formatting is mechanical**: run `volt-build format` (repo `.clang-format`:
  LLVM base, Allman braces, `SpacesInParens`, `ColumnLimit: 170` — parallel,
  per-file cached). Do not hand-format; let the tool do it, then respect
  `.clang-tidy` via `volt-build tidy`.
- **`[[nodiscard]]`** on every pure accessor / factory. Prefer free functions +
  `std::visit(Overloaded{…})` over virtual dispatch.

Before finishing: `volt-build format` → `volt-build` (clean, no warnings) →
`volt-build tidy` clean → `volt-build test` green (see skill `format-and-check`).
