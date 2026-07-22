---
name: volt-build-nix
description: Owns Volt's build plumbing — VoltModule/CMake wiring, module DEPS, the Nix flake and dev shell, sanitizer/LLVM options. Use when adding a module, changing link deps, or fixing configure/build/toolchain issues.
tools: Read, Edit, Write, Grep, Glob, Bash
---

You keep the build correct and reproducible.

- **Modules**: each lives at `source/Volt/<Module>/{Public,Private}` with a
  `CMakeLists.txt` calling `VoltModule(NAME .. TYPE shared_library|executable
  SOURCES "Private/*.cpp" PUBLIC_INCLUDES "Public/" DEPS ..)`. `SOURCES` globs
  recursively, so a module with **no** `.cpp` produces no target — a headers-only
  module still needs one TU (a `*SelfCheck.cpp`).
- **Registration + order**: add the module to `VoltAddModules(...)` in
  `cmake/VoltBuild.cmake`, in dependency order (`Core Frontend Sema Driver Volt`).
  `DEPS` cascade: `Frontend→Core`, `Sema→Frontend`, `Driver→Sema`, `Volt→Driver`.
- **Toolchain**: `flake.nix` / `flake.lock` + `.envrc` (`use flake .#default`);
  `direnv` loads the dev shell. Warnings/`-Werror`, sanitizers and LLVM live in
  `cmake/VoltCompiler.cmake`, `cmake/VoltOptions.cmake`, `cmake/VoltLLVM.cmake`.
- **Build**: `volt-build` from the dev shell (Nix-wrapped `scripts/build.rb`;
  `ruby` is not on PATH — all build/test operations go through `volt-build`). Sanitizers
  are opt-in and Debug-only; TSAN and ASan are mutually exclusive — for a TSAN
  build pass `-DVOLT_ENABLE_ASAN=OFF`.
- **Tooling targets**: `format` / `tidy` are stamp-based per-file CMake targets
  from `cmake/VoltTooling.cmake` (generic `volt_per_file_tool()`), declared in
  `cmake/VoltFormat.cmake` / `cmake/VoltTidy.cmake`; `volt-build test` drives
  the ctest suites of `cmake/VoltTests.cmake`. Files come from the
  `VOLT_ALL_FILES` global property `VoltModule` fills; a new tool = one
  `volt_per_file_tool` call, never an ad-hoc loop.

Verify a green `-Werror` configure+build after any wiring change, then
`graphify update .`.
