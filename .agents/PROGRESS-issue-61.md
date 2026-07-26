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
- [x] **Phase 1 — cache-key infrastructure (no cache consulted yet).** Done, verified.
- [ ] **Phase 2 — generic reflected serializer + frontend/sema cache.** Next up.
- [ ] Phase 3 — native precompiled static-archive cache.
- [ ] Phase 4 — shared-object artifact kind.
- [ ] Phase 5 — CLI flags as shared option group.

## Important note: only run `volt-build tidy` once, at the very end of the whole issue

`tidy` (clang-tidy) is extremely RAM-heavy. **Never run two `tidy` invocations
concurrently** (the user killed a session over this). Per-phase finishing
checks use `volt-build format test` only — `tidy` is reserved for the final
close-out once every phase above is done. See auto-memory
`feedback-tidy-heavy.md`.

## Unplanned but folded into this branch: LLVM backend scalar-parameter bug

Also folded in at the user's request ("fix it in this issue too, it's an
important regression"). Found while running the *LLVM-enabled* test variant
(`volt-build testing llvm test`, 223 tests — the plain `volt-build test` only
runs 209, no `LlvmIr`/`LlvmRun` cases) to sanity-check Phase 1 more broadly.

- **Symptom**: 14/223 tests failed (`LlvmIr`/`LlvmRun` for every `Codegen`
  sample), all with the same LLVM module-verification error:
  `` module verification failed in '_V4Bool3<=>': Load operand must be a
  pointer. `` on `%other` (Bool's `<=>` parameter).
- **Bisected**: reproduced identically on commit `443c101` (the commit right
  before both `06ef2bb` and `5a9a47f`, in a throwaway `git worktree`) — **not**
  caused by either of those two commits, and **not** related to issue #61.
  A separate, pre-existing bug.
- **Why CI never caught it**: `.github/workflows/ci.yml` ran plain
  `volt-build test` — CI has never built with `VOLT_ENABLE_LLVM=ON`, so
  `LlvmIr`/`LlvmRun` never ran in CI at all.
- **Root cause**: `LlvmEmitter.cpp::DefineMember` (the plain, non-generic
  method-definition path) bound every parameter into `Frame.Slots` as the raw
  SSA argument value (`Frame.Slots.emplace(Site, Arg)`), regardless of
  whether it was a scalar or an aggregate. Every later read of that parameter
  as an `Identifier` goes through `LoadPlace` → `EmitAddress` → unconditional
  `CreateLoad` on whatever `EmitAddress` returns — correct when the "address"
  is a real `alloca` (locals) or an already-pointer aggregate/`&block` param,
  wrong when it's a bare scalar value with no backing storage. `Optimizer.cpp`'s
  `VerifyModule` loop reports only the *first* broken function and returns
  (`llvm::verifyFunction` per-function loop, early `return false`), so
  `_V4Bool3<=>` was just the first stdlib body hit — the same defect almost
  certainly affects any scalar parameter read anywhere, not just `Bool`.
  `ClosureEmitter.cpp` and `MonoEmitter.cpp` already implement this correctly
  (`Arg->getType()->isPointerTy()` → keep as-is; else `SlotFor` + `CreateStore`
  an alloca) — `DefineMember` was the one path missing it.
- **Fix**: `source/Volt/Backend/BackendLLVM/Private/LlvmEmitter.cpp`'s
  parameter-binding loop in `DefineMember` now matches the same
  `isPointerTy()` / `SlotFor` pattern already used in the other two emitters.
  13/14 failures fixed by this alone.
- **The 14th failure was a different, genuine problem in the *sample*, not
  the compiler**: `samples/Codegen/TopLevelBasic.vl` relied on a top-level
  expression's value implicitly becoming the process exit code — never a
  real Volt semantic (there is no implicit top-level `return`; exiting needs
  an explicit call, like every other language, e.g. the stdlib's
  `@[External("libc","exit")]`). The user fixed the sample directly to call
  `libc_exit(x + y)` explicitly; `tests/golden/samples/Codegen/
  TopLevelBasic.vl{,.lowered}.golden` were regenerated to match the new
  source (`golden-update` target again), `.expected` (`exit=42`) needed no
  change since the computed value is unchanged.
- **Also added**: `llvm` to CI's build line
  (`.github/workflows/ci.yml`: `volt-build test` → `volt-build llvm test`)
  so this whole test class (and any future regression in it) is no longer
  invisible to CI.
- **Verified**: `volt-build testing llvm format test` — **223/223 tests
  pass** after both the emitter fix and the sample/golden fix.

## Unplanned but folded into this branch: Golden-fixture regression fix

Not part of the issue #61 design, but the user asked to fix it here since it's
an important, already-merged-to-`main` regression discovered while closing out
Phase 1:

- `5a9a47f` (`Fix(Frontend): Warning error with loc`, already on `main` via PR
  #68) added `Node.Loc = RangeSince(ParamBegin)` in
  `Frontend/Private/Parser/ParseDecl.cpp::ParseParameterList` — correct and
  intentional (every `Param` now carries a source range), but the 102 golden
  fixtures under `tests/golden/**` were never regenerated to match, so every
  `Golden.*`/`Golden.lowered.*` CTest case — and CI on `main` itself (run
  30212131901, PR #68) — has been failing since that commit landed.
- Fixed by running `cmake --build build/debug-testing --target golden-update`
  (see auto-memory `golden-update-command.md`) and diffing every changed file
  to confirm the only change is the expected `Param '<name>' @line:col`
  addition (spot-checked `RaiseDivergence.vl.lowered.golden` in full, plus a
  grep over the whole `tests/golden` diff for any line not matching that
  shape — none found). 102 `tests/golden/**` files are now modified,
  unstaged, alongside the Phase 1 diff below.
- This is unrelated to Phase 1's own code; it was surfaced only because
  Phase 1's finishing `volt-build format test` run turned up the failure.

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

## Phase 1 — done

**What changed:**
- New `source/Volt/Core/Public/Volt/Core/Support/ContentHash.hpp` +
  `Private/Support/ContentHash.cpp`: a small self-contained 64-bit FNV-1a
  content hash (`HashBytes`, `CombineHash`, `HashFile`, `HashFileTree`,
  `ToHex`). Deliberately **not** LLVM's `xxhash.h`/`MD5.h` (confirmed present
  at `llvm/Support/xxhash.h` — checked before writing this) — `Core` is the
  base module every other module depends on (`CMakeLists.txt` has zero
  `DEPS`), and pulling all of LLVM into it just to key a cache would be a
  layering violation. Not a cryptographic primitive; only needs to be
  change-sensitive, not collision-resistant against an adversary.
- `source/Volt/Driver/Private/Driver.cpp`:
  - `LoadStdLib`'s inline walk+sort was factored into a shared
    `CollectSortedStdlibFiles(LibDir)` (anonymous-namespace helper) so the
    frontend cache key hashes **the same walk** `LoadStdLib` compiles —
    the plan explicitly calls out not re-deriving a second, possibly
    inconsistent enumeration.
  - `ExecutableDir()` was split into `ExecutablePath()` (the `/proc/self/exe`
    readlink itself) + `ExecutableDir()` (its parent), so both stdlib
    resolution and the new build-fingerprint hashing share one
    `/proc/self/exe` read.
  - `CompilerBuildFingerprint()`: hashes the running binary's size + mtime
    (the MVP conservative choice from the plan — whole-binary invalidation
    on every compiler rebuild, refine later to just `Nodes.inl`/
    `TokenKind.inl`/`Pratt.inl`/`PassList.inl` if that proves too
    aggressive).
  - `ComputeFrontendCacheKey()`: `Hash(CompilerBuildFingerprint |
    HashFileTree(CollectSortedStdlibFiles(ResolveStdlibDir())))`.
  - `Driver::Driver()` now computes and stores this in a new
    `FrontendKey` field at construction (member-initializer, before the
    body runs) — **nothing consults it as a cache yet**, per the phase's
    scope.
  - New free function `Volt::Driver::ComputeNativeCacheKey(FrontendKey,
    TargetTriple, OptLevel, ArtifactKind, bLto)` — a plain function, not a
    `Driver` method, because the Driver doesn't know target/opt/artifact-kind
    (those are `BuildCommand`/backend concerns); a future caller (Phase 3/4's
    archive/`.so` cache build) combines them with a `Driver`'s new
    `FrontendCacheKey()` accessor. **Declared and exported
    (`DRIVER_EXPORT`) but not yet called anywhere** — intentionally inert
    API surface for Phase 3+, not dead code slated for deletion.
- `source/Volt/Driver/Public/Volt/Driver/Driver.hpp`: constructor moved
  out-of-line (was inline, now needs the hash computation from the `.cpp`);
  added `FrontendCacheKey()` const accessor and the `ComputeNativeCacheKey`
  declaration.

**Explicitly NOT done this phase, on purpose:**
- No cache file is read or written anywhere (Phase 2/3).
- The keys are computed but **not logged**. The plan said to log them (e.g.
  at `--verbose`), but no `--verbose`/quiet gate exists anywhere in the CLI
  yet (grepped — confirmed), and `Core::FLogger`'s default `MinLevel` is
  `Debug` (prints unconditionally — `Logger.cpp:63`). I initially added
  `FLogger::Debug(...)` calls in the `Driver` constructor and
  `BuildCommand.cpp`; this silently broke every `Golden.*` CTest case, since
  `volt parse`/`check`/`build` all construct a `Driver` and Golden tests diff
  stdout byte-for-byte. Removed both calls once diagnosed. **Do not re-add
  cache-key logging until a real verbosity flag lands (Phase 5)** — wire it
  through that, not an ad-hoc `FLogger::Debug` call.

**Verified:**
- `volt-build` (plain, no format/tidy) — clean, no warnings, `Core`/`Driver`/
  `Volt` modules rebuilt successfully.
- Determinism: ran `volt_d check --input samples/Sema` twice in a row —
  identical `FrontendCacheKey` both times (confirmed via a temporary log
  line during development, since removed).
- Change-sensitivity: appended a byte to `source/Lib/Mixins/Arithmetic.vl`,
  key changed; reverted the file, key returned to the original value.
- Full `volt-build format test` — **209/209 tests passed** (after also
  fixing the unrelated golden-fixture regression below, which the first
  attempt surfaced).
- `tidy` was **not** run this phase (reserved for the end of the whole
  issue — see the note above).

## Phase 2 — next

Per the plan file's §"Frontend/Sema cache (what gets cached, and how)" and
§"Phase 2 — Generic reflected serializer + frontend/sema cache (highest-risk
phase)":

- Build a generic reflected `Serialize<T>`/`Deserialize<T>` pair in `Core`
  (new header, alongside `Meta::Reflect.hpp`), walking any `Meta::Reflected`
  aggregate via `Meta::ForEachField` — covers every `ExprNode`/`StmtNode`/
  `DeclNode`/`TypeNode` alternative and `Sema::PassStats` with zero
  per-node code, the same mechanism the printer/walker already use
  (`meta-first.md`).
- Hand-written leaf primitives only for: `Core::TypedId<Tag>` (as `u32`),
  `Core::Symbol` (as interned index), `Core::SmallVec`,
  `std::variant`/`std::optional`/`std::string`/`std::vector`/
  `std::unordered_map`.
- One hand-written `Core::Arena<T,IdType>` (de)serializer, generic over `T`
  via those same primitive calls — arena replay reproduces identical `Id`s
  with no remap table (`ast-value.md`'s `std::vector`-backed arena).
- `StringInterner`: serialize as an ordered list, rebuild by replaying
  `Intern()` in original order — reproduces identical `Symbol` values.
- **`ScopeTable` (`Scopes`) must also round-trip** — see the "blind spots"
  reminder #1 below; easy to forget since the plan's main body doesn't
  mention it, only the addendum does.
- `Sema::UnitCallees::CalleeEntry.Decl` (raw `const Member*` into the
  build-wide `TypeStore`) needs the one hand-written two-phase fixup:
  serialize `(Owner NominalId, member Symbol)`, re-resolve the pointer after
  `TypeStore` is loaded.
- Wire `Driver::CompileRefs`'s stdlib sub-path: on a frontend-cache hit,
  deserialize straight into `Units`/`TypeStore`/`InterfaceRegistry`,
  skipping `ParseOne`→seam→`RunSemaOne` for stdlib `SourceRef`s; on a miss,
  run the existing pipeline unchanged, then serialize the result. This is
  where `FrontendCacheKey` (Phase 1, now available via `Driver`) finally
  gets **consulted**, not just computed.
- Respect reminder #2 below: stdlib-cache loading must happen strictly
  before any user file is lexed/parsed, so `StringInterner` order replays
  identically before user symbols are interned on top.
- Location for the cache file itself: `$XDG_CACHE_HOME/volt/stdlib/
  <FrontendCacheKey>/frontend.cache` (fallback `~/.cache/volt/...`),
  written via write-to-`tmp.<pid>`-then-`rename()`. A missing/corrupt/
  version-mismatched cache file is always a miss (recompute + warn), never
  a crash — this is also the natural point to introduce the first real
  `Core::FLogger::Warn` for this feature (cache miss/corruption), since it's
  conditional on an actual anomaly rather than always-on chatter.

**Verify:** `tests/AstInvariant.cmake` passes identically cached vs. fresh; a
new round-trip test asserting a cached stdlib build's `TypeStore`/
`InterfaceRegistry` state is equivalent to a from-scratch build's (same
`NominalId`/`Member` shapes, same `Deferred` bits per `SemaType.hpp`'s
`UnitTypes::MarkDeferred`). This is the highest-risk phase in the whole
plan — go slow, verify the round-trip before wiring it into the live
`CompileRefs` path, and consider landing it behind a review-gate flag first
per the plan's rollout note.

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
