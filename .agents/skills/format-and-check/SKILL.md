---
name: format-and-check
description: The finishing checklist for any Volt change — volt-build format, a clean -Werror volt-build build, volt-build test, and a graphify refresh. Use before calling a compiler change done. Never run volt-build tidy; clang cannot parse this codebase.
---

# Format & check

Run these before finishing a change. Any failure means the change is not done.
Every step goes through `volt-build`; the tooling targets are parallel and per-file cached, so
re-runs only touch what changed.

1. **Format, Build & Test** (preferred single-command execution):
   ```sh
   volt-build format test         # Formats, builds -Werror and runs tests in one pass
   ```
   *Note: `volt-build` orchestrates all actions in a single pipeline. Prefer `volt-build format test` over chaining multiple commands with `&&`.*

2. **Individual steps** (if running separately):
   ```sh
   volt-build format              # Allman, SpacesInParens, col 170
   volt-build                     # Zero warnings, zero errors → build/bin/Volt_d
   volt-build test                # Implies VOLT_ENABLE_TESTING=ON, ctest in parallel
   ```
3. **Do _not_ lint.** `volt-build tidy` is **not** part of this checklist — see
   "Why there is no lint step" below. Skip it, and ignore clangd too.
4. **Regenerating goldens** (if AST output changes intentionally):
   ```sh
   cmake -DUPDATE=1 -P tests/GoldenTest.cmake   # or the `golden-update` target
   ```
5. **Parallel safety** (only if you touched the Driver or a pass's shared state):
   `volt-build debug tsan` and compile a multi-file circuit
   (`volt check samples/Circuits/DiamandDeps`) — expect no TSAN reports.
6. **Refresh the graph**:
   ```sh
   graphify update .              # AST-only, no API cost
   ```

## Why there is no lint step

`clang-tidy` and `clangd` are **clang** front-ends. Volt is a **GCC** codebase:
the AST, the printers and every generic walk are built on C++26 static
reflection (P2996) — `#include <meta>`, `^^T`,
`std::meta::nonstatic_data_members_of`, `template for`. GCC implements that
today; **clang does not**, so it cannot parse `Meta/Reflect.hpp`, and therefore
cannot parse any TU whose include closure reaches it — which is most of
`Frontend/`, `Sema/` and `Driver/`.

What you see instead is a flood of **bogus** diagnostics from a failed parse:

```
No type named 'ExprList' in namespace 'Volt::Frontend'
no type named '__index_type' in 'std::__detail::__variant::_Variant_base<…>'
no member named '_M_u' in 'std::variant<…>'
```

None of these are real. Do not "fix" them, do not work around them, and do not
treat them as regressions of your change.

**`volt-build` (GCC, `-Werror`) is the only ground truth.** Revisit this step
once clang ships P2996.
