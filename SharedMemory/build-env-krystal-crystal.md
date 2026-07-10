---
name: build-env-krystal-crystal
description: How to build Volt — krystal wrapper needs crystal on PATH; nix store location
metadata: 
  node_type: memory
  type: reference
---

Volt is built with the `krystal` wrapper (Nix flake input `github:Yutsuna/Krystal`), not vanilla `crystal`. Commands: `krystal -R -f -o OUT` (fast/-O3, default), `krystal -r -f -o OUT` (release), `krystal -s` (specs), `-f` bypasses cache.

Do NOT manually export/prepend a nix-store `crystal` to PATH — user upgraded `krystal` to bundle `crystal` directly (plus the `mold` linker for faster builds/links). It's available in the tool shell as-is; the old PATH-export workaround (a specific nix-store crystal-1.19.1 path) is obsolete and was explicitly corrected by the user ("pas besoin d'export le path. krystal est déjà load dans ton environnement de dev").

Benchmark harness: `./Benchmarks/bench.sh` (timings, falls back to `time` when `hyperfine` isn't installed) / `--check` (output only). See [[volt-perf-milestone]].
