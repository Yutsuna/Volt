---
name: build
description: Build Volt with the volt-build tool from the Nix dev shell — the only supported way to configure, compile, sanitize, and run the compiler. Use instead of invoking cmake, ninja, or ruby directly.
---

# Build

The dev shell (`nix develop`, see `nix/shell.nix`) puts **`volt-build`** on PATH:
a Nix-wrapped Ruby pipeline (`nix/volt-build.nix` → `scripts/build.rb`). It owns
CMake configuration, ninja invocation, ccache, and mold. **Never** run `cmake`,
`ninja`, or `ruby scripts/build.rb` by hand — `ruby` is not even on PATH outside
the wrapper, and stray build dirs go stale.

## Commands

```sh
volt-build                    # Debug build (default) → build/bin/Volt_d
volt-build release            # Release build         → build/bin/Volt
volt-build clean              # wipe build dir + cache
volt-build run -- <args>      # build then run the binary with <args>
volt-build format             # clang-format, parallel + per-file cache
volt-build tidy               # clang-tidy,   parallel + per-file cache
volt-build test               # build, then all ctest suites in parallel
```

Feature flags combine freely with a build type:

```sh
volt-build debug asan run -- --verbose   # AddressSanitizer
volt-build debug ubsan                   # UndefinedBehaviorSanitizer
volt-build debug tsan                    # ThreadSanitizer (Driver / pass changes)
volt-build testing                       # VOLT_ENABLE_TESTING=ON (ctest targets)
```

## Tooling targets (format / tidy)

`format` and `tidy` are CMake targets built on **`cmake/VoltTooling.cmake`**:
`volt_per_file_tool()` writes one stamp per file under `build/tooling/<target>/`,
so Ninja provides the cache (a file is re-processed only when it — or the tool's
config file — changed) and the parallelism (one job per file) for free. Files
are registered by `VoltModule` (`VOLT_ALL_FILES`: `.cpp` + `.hpp` + `.inl`);
manifests keep their `clang-format off` guards.

`tidy` (`cmake/VoltTidy.cmake`) additionally absorbs two toolchain realities:
it republishes the compile database stripped of GCC's `-freflection` (unknown
to clang tooling; mtime-stable so reconfigures keep the cache), and it skips
TUs whose include closure reaches `Meta/Reflect.hpp` — clang cannot parse
P2996 — detected with the preprocessor (`-MM -MG`), never a hardcoded list.

## Rules

- A change is not done until `volt-build` exits `[ OK ]` — the build carries
  `-Werror`, and the Core/Frontend self-checks are `static_assert`s, so a clean
  compile *is* the self-test.
- Touched the Driver or shared pass state? Also run `volt-build debug tsan` and
  compile a multi-file circuit (see `format-and-check`, step 5).
- The finishing checklist (format → build → tidy → test → graphify) is the
  `format-and-check` skill; this skill documents the tool itself.
