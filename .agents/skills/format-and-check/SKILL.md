---
name: format-and-check
description: The finishing checklist for any Volt change — an IDE-driven build and test, the `format` configuration at the end of a phase, `tidy` at the end of an epic, and a graphify refresh. Use before calling a compiler change done.
---

# Format & check

Run these before finishing a change. Any failure means the change is not done.
Build and test through the IDE (CLion). Never "run" a module configuration —
a module (`Core`, `Frontend`, `Sema`, `Driver`, `BackendLLVM`, …) is a library
target (`.so`/`.a`), not an executable, and the IDE will refuse to launch it.
Run an executable or test configuration instead (a `*Test` target, or
`All CTest`) — the IDE builds every dependency first. The tooling targets
(`format`, `tidy`) are parallel and per-file cached, so re-runs only touch
what changed.

1. **Build & test.** Run a test configuration (e.g. `All CTest`, or a single
   `*Test` target while iterating on one area). This builds the project under
   `-Werror` — warnings are treated as failures — then runs the suite.
2. **Format.** Run the `format` configuration (Allman, `SpacesInParens`,
   column 170). Do this **once, at the end of a phase**, not after every
   edit — it reformats every touched file in one pass, so running it earlier
   only wastes time.
3. **Regenerating goldens** (if AST output changes intentionally):
   ```sh
   cmake -DUPDATE=1 -P tests/GoldenTest.cmake   # or the `golden-update` target
   ```
4. **Parallel safety** (only if you touched the Driver or a pass's shared state):
   a TSAN build (Debug, `-DVOLT_ENABLE_ASAN=OFF`), through the IDE, over a multi-file circuit
   (`volt check samples/Circuits/DiamandDeps`) — expect no TSAN reports.
5. **Refresh the graph**:
   ```sh
   graphify update .              # AST-only, no API cost
   ```
6. **Static analysis & Tidy.** Run the `tidy` configuration **only at the end
   of an epic**, once every phase is complete — never mid-phase. It is
   expensive and, under C++26, sometimes flags things that are not actually a
   problem; prefer the IDE's own diagnostics (clangd / inspections) during
   day-to-day development.
