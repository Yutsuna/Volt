# Rule: build-performance — local incremental vs. clean unity builds

The Volt build system uses a **dual-mode strategy** to balance fast local inner-loop iteration with fast clean builds in CI or release packaging.

## 1. Local Development (Default: Non-Unity)

For day-to-day development (`volt-build`, `volt-build debug run`, `volt-build format test`):

- **`VOLT_UNITY_BUILD` is `OFF` by default.**
- **Per-file translation units:** Every `.cpp` file compiles independently across all available CPU cores.
- **Granular caching:** Modifying a single `.cpp` file (e.g. `EnumLowering.cpp`) only recompiles that single file (~1–2 seconds), leveraging precompiled headers (`VoltPCH`), Split DWARF (`-gsplit-dwarf`), and `ccache`.

## 2. Clean / CI / Release Builds (`unity`)

For 100% clean builds from scratch (`volt-build unity`, `volt-build release unity`):

- **`VOLT_UNITY_BUILD` is opt-in via `unity`.**
- **Batching:** Source files are grouped in batches of 8 (`CMAKE_UNITY_BUILD_BATCH_SIZE 8`) to avoid RAM/CPU bottlenecks on single threads with C++26 templates.
- **Header parsing reduction:** Reduces total clean build time from scratch (e.g. ~2m06s vs ~2m37s) by eliminating redundant header parsing across TUs.

## 3. Nix Environment & Script Caching

- `nix/volt-build.nix` wraps `scripts/` into the `voltBuild` package inside `nix/shell.nix`.
- **Note:** Any edit to Ruby scripts in `scripts/` requires re-entering the Nix dev shell (`nix develop .`) so Nix rebuilds the store path for `voltBuild`.
