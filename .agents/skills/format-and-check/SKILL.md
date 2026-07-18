---
name: format-and-check
description: The finishing checklist for any Volt change — clang-format, clang-tidy, a clean -Werror ninja build, the corpus parse sweep, and a graphify refresh. Use before calling a compiler change done.
---

# Format & check

Run these before finishing a change. Any failure means the change is not done.

1. **Format** every file you touched:
   ```sh
   clang-format -i <files>        # repo .clang-format: Allman, SpacesInParens, col 170
   ```
2. **Build clean** under `-Werror`:
   ```sh
   cmake -S . -B build -G Ninja   # only if CMake lists changed
   ninja -C build                 # zero warnings, zero errors
   ```
3. **Lint** (respect `.clang-tidy`): resolve or justify every diagnostic on
   touched files. clang-tidy does not understand GCC's `-freflection`, so
   point it at a stripped compile DB:
   ```sh
   mkdir -p build/tidy
   sed 's/ -freflection//' build/compile_commands.json > build/tidy/compile_commands.json
   clang-tidy -p build/tidy <touched .cpp files>
   ```
   TUs that include `Volt/Core/Meta/Reflect.hpp` cannot be parsed by clang
   tooling at all until LLVM ships P2996 — diagnostics there are noise;
   justify instead of chasing them.
4. **Corpus sweep** — every sample and stdlib file must parse:
   ```sh
   for f in $(find samples source/Lib -name '*.vl' -o -name '*.vlx'); do
     ./build/bin/Volt "$f" >/dev/null 2>&1 || echo "FAIL $f"
   done
   ```
5. **Parallel safety** (only if you touched the Driver or a pass's shared state):
   configure `-DCMAKE_BUILD_TYPE=Debug -DVOLT_ENABLE_TSAN=ON -DVOLT_ENABLE_ASAN=OFF`
   in a separate build dir and compile a multi-file circuit
   (`--circuit samples/Circuits/DiamandDeps/Project.vl`) — expect no TSAN reports.
6. **Refresh the graph**:
   ```sh
   graphify update .              # AST-only, no API cost
   ```
