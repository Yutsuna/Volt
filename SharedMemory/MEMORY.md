# Memory index

- [Volt Core stdlib](volt-core-stdlib.md) — hybride embarqué+VOLT_CORE, pur Volt sans @[Primitive], shadowing via mark_redefinable; étapes 1-2 faites, étape 3 (dé-hardcode String) restante; next issue: aggressive inlining

- [Volt perf milestone](volt-perf-milestone.md) — phased Tier-0 VM speedup; ALL 6 phases done, fib 16.5s→~1.0s (on par w/ Lua), primes 1.09s→~0.07s (beats Lua), target met; P6 explicit frame stack = flat/structural, to_c_arg bug fixed
- [Build env: krystal/crystal](build-env-krystal-crystal.md) — how to build Volt; crystal PATH quirk + nix store path
- [Crystal GC atomic Value payload](crystal-gc-atomic-value-payload.md) — CRITICAL: Value payload must stay Pointer(Void) or GC collects live Strings/objects; + enum `.nil?` shadowing trap; + assign-ident aliasing/RAII fix
- [Mistral bug fixes](volt-mistral-bug-fixes.md) — VOLT-001..004 fixed (next kw, nil literal, T? nilable, circular-def false positive), VOLT-005 skipped; bin/Volt staleness gotcha for specs
