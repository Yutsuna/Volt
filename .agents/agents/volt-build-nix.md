---
name: volt-build-nix
description: Owns Volt's build plumbing — Meson wiring, module DEPS, the Nix flake and dev shell, sanitizer/LLVM options. Use when adding a module, changing link deps, or fixing configure/build/toolchain issues.
tools: Read, Edit, Write, Grep, Glob, Bash
---

You keep the build correct and reproducible.

- **Modules**: each lives at `source/Volt/<Module>/{Public,Private}` with a
  `meson.build` declaring `library(...)` and exported dependencies.
- **Registration + order**: register the module via `subdir('<Module>')` in
  `source/Volt/meson.build`, in dependency order (`Core Frontend MiddleEnd Driver Volt`).
  Dependencies cascade: `Frontend→Core`, `MiddleEnd→Frontend`, `Driver→MiddleEnd`, `Volt→Driver`.
  **Order is load-bearing**: a backend the Driver links must be `subdir`'d
  *before* `subdir('Driver')`, or its `*_dep` variable does not yet exist when
  the Driver configures. `BackendLlvmIr`, `BackendLLVM` and `BackendJIT` sit
  there; `BackendWASM` (linked by nothing) sits after.
- **Toolchain**: `flake.nix` / `flake.lock` + `.envrc` (`use flake .#default`);
  `direnv` loads the dev shell. Warnings/`-Werror`, sanitizers and LLVM live in
  `meson.build` and `meson/meson.build`.
- **Options**: `enable_llvm` gates `BackendLlvmIr` + `BackendLLVM` (`volt build`);
  `enable_jit` gates `BackendJIT` (`volt run`, `volt repl`) and is subordinate to
  `enable_llvm` — `enable_jit=true` with `enable_llvm=false` must `warning()` and
  behave as false, not fail configure. `llvm_dep` is declared once in
  `meson/meson.build` and shared by the three LLVM modules; it carries no
  `modules:` list, so it resolves to the monolithic `libLLVM` and ORC comes with
  it at no extra wiring.
- **Stdlib artifact**: `volt build-stdlib` warms the native stdlib cache under
  `~/.cache/volt/stdlib/<FrontendKey>/native/`. It is a cache warm-up, not a
  build target — do not wire it into meson.
- **Build**: through the IDE (CLion) or CLI (`meson setup build && ninja -C build`), which drives Meson/Ninja directly — the
  old Ruby-based wrapper script has been removed. Never "run" a module
  configuration (a library target, `.so`/`.a`); run an executable or test
  configuration instead, which builds its dependencies first. Sanitizers are
  opt-in and Debug-only; TSAN and ASan are mutually exclusive — for a TSAN
  build pass `-Denable_asan=false -Denable_tsan=true` at configure time.
- **Tooling targets**: `format` is declared in `meson/meson.build` (`run_target('format', ...)`),
  and test suites are declared in `tests/meson.build` (run via `meson test` or `ninja tests`).

Verify a green `-Werror` configure+build through the IDE after any wiring change, then
`graphify update .`.
