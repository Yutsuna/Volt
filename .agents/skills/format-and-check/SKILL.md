---
name: format-and-check
description: The finishing checklist for any Volt change — volt-build format, a clean -Werror volt-build build, volt-build tidy, volt-build test, and a graphify refresh. Use before calling a compiler change done.
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
3. **Static analysis & Tidy** (optional or for final verification):
   ```sh
   volt-build tidy
   ```
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
