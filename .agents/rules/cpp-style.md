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
- **Formatting is mechanical**: run `volt-build format` (Allman braces, `SpacesInParens`,
  `ColumnLimit: 170` — parallel, per-file cached). Do not hand-format; let the tool do it.
- **`[[nodiscard]]`** on every pure accessor / factory. Prefer free functions +
  `std::visit(Overloaded{…})` over virtual dispatch.

Before finishing: `volt-build format` → `volt-build` (clean, no warnings) →
`volt-build test` green (see skill `format-and-check`).

## Do not run `volt-build tidy` — and ignore clangd

Both are **clang** front-ends, and Volt is a **GCC** codebase: the AST, the
printers and every generic walk are built on C++26 static reflection (P2996) —
`#include <meta>`, `^^T`, `std::meta::nonstatic_data_members_of`,
`template for`. GCC ships that today; clang does not implement it yet, so
clang cannot parse `Meta/Reflect.hpp` at all.

The fallout is that clang-tidy and clangd report a flood of **bogus** errors on
files they simply failed to parse — typically `No type named 'ExprList' in
namespace 'Volt::Frontend'`, `no type named '__index_type'`, `no member named
'_M_u' in 'std::variant<…>'`. These are artefacts of a broken parse, not
findings about the code.

**`volt-build` (GCC, `-Werror`) is the only ground truth.** Judge doneness by
`volt-build format` → `volt-build` → `volt-build test`, and disregard whatever
the editor underlines in red. Revisit this once clang implements P2996.
