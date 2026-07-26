# Progress — Issue #61 (Pre-compile stdlib as shared/static lib)

Branch: `Perf/Volt-pre-compile-std-lib-as-shared-lib`. Full design plan (context,
grounding facts, design, phased delivery, verification) lives in
`/home/Yutsuna/.claude/plans/tu-as-toute-la-eventual-brook.md` — **read that file
first**, this doc only tracks phase-by-phase status and handoff notes.

Working agreement with the user: stop at the end of every phase, write the
complete context here (what changed, why, what's verified, what's next), so a
fresh session can resume with no memory of this one.

## Status

- [x] **Phase 0 — stdlib path resolution fix.** Done, verified.
- [ ] **Phase 1 — cache-key infrastructure (no cache consulted yet).** Next up.
- [ ] Phase 2 — generic reflected serializer + frontend/sema cache.
- [ ] Phase 3 — native precompiled static-archive cache.
- [ ] Phase 4 — shared-object artifact kind.
- [ ] Phase 5 — CLI flags as shared option group.

Task tracker: tasks #1 (done) through #6 (pending) in this session's task list
mirror the phases above 1:1, with `blockedBy` chaining 3←2, 4←3, 5←4. A fresh
session should recreate/consult these via TaskList if the tracker doesn't
carry over, but this file is the durable source of truth either way.

## Phase 0 — done

**What changed:**
- `source/Volt/Driver/Private/Driver.cpp`: `Driver::LoadStdLib` used to
  hardcode `fs::path LibDir = "source/Lib"`, resolved relative to the
  process's CWD — `volt build`/`check` only found the stdlib when invoked
  from the repo root. Replaced with `ResolveStdlibDir()`, priority order:
  1. `VOLT_STDLIB_DIR` env var (explicit override, dev/test escape hatch).
  2. `<exe_dir>/../share/volt/Lib` via a new `ExecutableDir()` helper
     (`/proc/self/exe` readlink — Linux-only, matches the project's current
     platform scope) — a conventional install layout, so this starts working
     for free the day a real `install()` rule exists (none does yet).
  3. `VOLT_DEV_STDLIB_DIR` — a compile-time constant baked into the `Driver`
     module pointing at `${CMAKE_SOURCE_DIR}/source/Lib`, so a dev build
     always finds the real stdlib regardless of invocation CWD.
- `source/Volt/Driver/CMakeLists.txt`: added
  `PRIVATE_DEFINES VOLT_DEV_STDLIB_DIR="${CMAKE_SOURCE_DIR}/source/Lib"`.

**Verified:**
- `volt-build debug` — clean, no warnings.
- `cd /tmp && .../build/debug/bin/volt_d check --input <repo>/samples/Sema` —
  succeeded (`20 file(s) type-checked, 40 exported decl(s)`) from a CWD with
  no `source/Lib` at all, proving the fix.
- One clang-tidy fix needed and applied along the way:
  `modernize-return-braced-init-list` on the two `return fs::path(...)`
  lines — changed to `return { ... };`.

**Not yet run:** `volt-build format tidy` as a final pass, and
`volt-build test` — the user interrupted before this Bash call (unrelated to
the change's correctness; just hadn't been run). **Do this first in the next
session** before starting Phase 1, to close out Phase 0 properly:
```sh
volt-build format tidy test
```

## Phase 1 — next

Per the plan file's §"Cache keys and on-disk layout" and §"Phase 1": add a
content-hashing utility and compute two keys at `Driver` construction time,
**without** consulting or writing any cache file yet (that's Phase 2/3) —
this phase is purely about proving the key itself is correct in isolation.

- `FrontendCacheKey = Hash(CompilerBuildFingerprint | sorted(source/Lib/** path+content))`
- `NativeCacheKey = FrontendCacheKey | TargetTriple | OptLevel | ArtifactKind(static|shared) | LTO`
- `CompilerBuildFingerprint` for the MVP: hash of the running `volt` binary
  itself (mtime+size, or a build-id) — conservative but safe; refine later to
  hash just `Nodes.inl`/`TokenKind.inl`/`Pratt.inl`/`PassList.inl` if whole-
  binary invalidation proves too aggressive in practice.
- Check whether LLVM (already a dependency via BackendLLVM) vendors a usable
  hash (e.g. it ships xxhash/BLAKE3 internally for its own caching) before
  pulling in a new hashing dependency for `Core`.
- Where the key-computation code should live: likely a new
  `Core::Support` header (e.g. `Core/Support/ContentHash.hpp`) — hashing a
  file and hashing a sorted file tree are generic utilities, not
  Driver-specific, and this is exactly the kind of primitive future Phase
  2/3 code (serialization, native cache) will also want to depend on from
  `Core` without a layering violation.
- `ResolveStdlibDir()` (Phase 0, `Driver.cpp`) is the function that must feed
  the sorted-file-tree hash — reuse it, don't re-walk `source/Lib` with a
  second, possibly-inconsistent method.
- Log the computed keys (e.g. at `--verbose`, check how `BuildCommand`/
  `Main.cpp` currently expose a verbosity flag, if any) but take no other
  action this phase.

**Verify:** same `source/Lib/**` content → identical key across two separate
`volt` invocations; touching one byte of one stdlib file → key changes. This
can be a small, fast unit-level test on the hashing utility alone — it does
not need a full compiler invocation, per the plan's verification section.

## Reminders carried from the plan file's "blind spots" addendum

These were added by the user directly into the plan file and must be
respected in the phases that touch them:

1. `Sema::ScopeTable` (`Scopes`) must also be serialized in Phase 2 — it's
   part of `Backend::UnitView` and closure-frame synthesis depends on it.
2. Stdlib-cache loading (Phase 2) must happen **strictly before** any user
   file is lexed/parsed in a given `Driver` run, so `StringInterner` symbol
   order for the stdlib replays identically before user symbols are interned
   on top.
3. Phase 3's precompiled stdlib archive can itself contain monomorphized
   generic instantiations (e.g. `Array<UInt8>` used internally by the
   stdlib) — if user code independently instantiates the same generic with
   the same type, the two must not collide at link time. Either record such
   symbols in `<NativeCacheKey>.meta` so the user build's monomorphizer
   knows to skip re-emitting them, or emit them `linkonce_odr`/comdat so the
   linker deduplicates automatically.
4. Stdlib units must occupy ordinals `0..S-1` in `Driver::Units` on both a
   cold and a cache-hit path, with user units always starting at ordinal
   `S` — this is what keeps `NominalType::Unit`/`Member::Unit` consistent
   whether or not the stdlib was loaded from cache.
