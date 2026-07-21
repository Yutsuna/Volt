---
name: format-and-check
description: The finishing checklist for any Volt change — volt-build format, a clean -Werror volt-build build, volt-build tidy, volt-build test, and a graphify refresh. Use before calling a compiler change done.
---

# Format & check

Run these before finishing a change. Any failure means the change is not done.
Every step goes through `volt-build`; the tooling targets are parallel and per-file cached, so
re-runs only touch what changed.

1. **Format**:
   ```sh
   volt-build format              # Allman, SpacesInParens, col 170
   ```
   Covers `.cpp` + `.hpp` + `.inl`; the manifests are protected by format off guards.
2. **Build clean** under `-Werror`:
   ```sh
   volt-build                     # zero warnings, zero errors → build/bin/Volt_d
   ```
3. **Lint**: resolve or justify every diagnostic on
   touched files:
   ```sh
   volt-build tidy
   ```
   The target already handles the toolchain gaps: the compile database is
   republished without GCC's `-freflection`, and TUs whose include closure
   reaches `Meta/Reflect.hpp` are skipped automatically — no manual `sed`, no justification needed for
   those.
4. **Test suite** — golden sweep + stdlib corpus + zero-hardcode guard:
   ```sh
   volt-build test                # implies VOLT_ENABLE_TESTING=ON, ctest in parallel
   ```
   An intended AST-output change means regenerating the goldens, then re-running:
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
