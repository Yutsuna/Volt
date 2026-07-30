# Removing `@[ExceptionRoot]` and `@[Unhandled]`

The two annotations `rules/zero-hardcode.md` lists as *remaining debt*. This
file records the measured state, the design decision, and the phased plan.

Status: **decided, not yet implemented.** Nothing in this file is committed
behaviour until the phases below are done.

## 1. What the two annotations actually do today

`@[ExceptionRoot]` is not used for three things, as the first sketch assumed,
but for **five**:

| # | Use | Site |
|---|---|---|
| 1 | sizes the EH buffer (max over the root's descendants) | `Backend/BackendLLVM/Private/ExceptionEmitter.cpp:103` |
| 2 | checks a `rescue` filter descends from the root | `Sema/.../TypeChecker/ExprInferencer.cpp:704` |
| 3 | supplies the default filter type of a bare `rescue` | `Sema/.../TypeChecker/ExprInferencer.cpp:713` |
| 4 | is the constructed type of the `raise "msg"` desugar | `MakeExceptionConstructor`, `ExprInferencer.cpp:254` |
| 5 | owns the `@[Unhandled]` hook | `ExceptionEmitter.cpp:136` |

`@[Unhandled]` marks the member the C entry shell calls when the tag is still
set after every unit init has run (`LlvmEmitter.cpp:561`). It is pure
"call this member" — the exact shape `rules/zero-hardcode.md` forbids.

Plumbing behind them: `Member::bUnhandled` (`TypeStore.hpp:116`), set in
`TypeBinder.cpp:448`; `TypeStore::{Set,Get}ExceptionRoot` +
the `ExceptionRoot` field (`TypeStore.hpp:504-520, 642`), serialised at
`TypeStoreSerialize.cpp:26,64`, cleared at `TypeStore.hpp:611`.

## 2. The EH model these sit on — tier 1, not Itanium

Load-bearing, because it invalidates a tempting argument (§3.2). Volt's
exceptions are **tier 1** (`.agents/backend/llvm.md:391`):

- `raise` writes a `NominalId` tag plus a `memcpy` of the object into a
  thread-local global (`volt.exc.storage`), then takes the *poisoned path* —
  an early `ret` of the type's zero value (`ExceptionEmitter.cpp:233`).
- Propagation is the **caller's post-call check** (`EmitExceptionCheck`,
  `ExceptionEmitter.cpp:265`), wired into `EmitResolvedCall` / `EmitIndirectCall`.
  Tier 1 never crosses a call boundary any other way.
- There is no `invoke`, no `landingpad`, no personality, no
  `__cxa_allocate_exception`. Itanium zero-cost EH is tier 2, still future
  (`llvm.md:446`).

Consequence that matters for phase B: because the check is wired into
`EmitResolvedCall`, a call to an `@[External]` function **also** gets its
post-call check. That is what makes a Volt-side prelude able to catch an
exception raised inside compiler-emitted unit inits.

## 3. Three corrections to the original proposal

### 3.1 `raise "msg"` → `RuntimeError.new(...)` is a regression

The sketch proposed dropping the root and finding `RuntimeError` by ordinary
global-scope lookup. That puts the literal string `"RuntimeError"` into
`ExprInferencer.cpp`. Today `MakeExceptionConstructor` reads the root's name
*out of the TypeStore* (`Types.Text(...)`, `ExprInferencer.cpp:260`) and no
Volt type name enters C++ at all. The swap would trade an annotation for a
hardcoded Volt type name and break the grep guardrail over `source/Volt/Sema`.
Strictly worse. Refused.

### 3.2 The `__cxa_allocate_exception(sizeof(T))` argument describes tier 2

In tier 1 there is no per-`raise` allocation to size — there is one
thread-local global whose size is a compile-time max. "The sizing problem
disappears" only becomes true after landingpad + personality land, which is a
far larger project than removing two annotations. What *would* remove the root
from sizing today is taking the max over every type in the store instead of
over the root's descendants — correct, but only acceptable if `raise` accepts
any type at all (§4).

### 3.3 "Rust/Swift/Crystal have no annotation" is not accurate

Rust finds these through **lang items / attributes** — `#[lang = "start"]`,
`#[lang = "eh_personality"]`, `#[panic_handler]`. Crystal's compiler refers to
`Crystal.main` by name. The industry standard is not "zero annotation"; it is
that the annotation states **what something is**, not what the compiler must
do. That is also this repo's own test, and it is what separates the two
annotations: a root *is* a fact about a type; `@[Unhandled]` is an imperative.

## 4. The decision

The semantics of `raise <expr>` and the fate of the root are **one decision**,
not two:

- Ruby refuses `raise 42` with a runtime `is_a?(Exception)` test inside
  `Kernel#raise`. Volt is statically typed, so the equivalent is a
  **compile-time** refusal — which requires knowing a root.
- C++ accepts `throw 42` precisely because it has **no privileged root**.

**Decided: Ruby-strict semantics, root kept, annotation removed.**

`raise <expr>` requires the static type of `<expr>` to descend from the root,
refused at compile time otherwise. The root stops being an annotation and
becomes a **node-kind claim** — the same mechanism that killed `@[Apply]`:

```volt
@[Literal( RaiseExpr )]
class Exception
```

`TypeStore::LookupNodeKind( "RaiseExpr" )` replaces `GetExceptionRoot()` at all
five sites. A node kind is the compiler's own vocabulary, not a Volt name —
identical in status to `NilLiteral` (TypeCompat), `PointerType`
(ExprInferencer) and `FuncType` (MemberResolver). No language semantics change,
and the closed list of annotations stays closed.

`@[Unhandled]` is removed separately, by moving the top-of-program handler into
**Volt code in a prelude** (§5.2).

### Rejected alternatives

- **C++-libre (`raise` accepts anything).** Genuinely dissolves the root at all
  five sites, but changes the language: `raise "msg"` would raise a `String`
  rather than construct an exception, and `rescue e : Int32` becomes legal.
  Not worth the semantic churn.
- **Keep `@[ExceptionRoot]`, fix only `@[Unhandled]`.** Leaves the guardrail
  grep printing four lines instead of three.

## 5. Plan

### Phase A — `@[ExceptionRoot]` → `@[Literal( RaiseExpr )]`

1. `source/Lib/Primitives/Exception.vl`: swap the annotation on `class Exception`.
2. `Sema/Private/Layout/TypeBinder.cpp`: delete the `AnnoName == "ExceptionRoot"`
   branch (~`:598`). The `Literal` branch above it already handles the claim,
   including the "already claimed by another type" refusal.
3. `Sema/Public/Volt/Sema/Layout/TypeStore.hpp`: delete `SetExceptionRoot`,
   `GetExceptionRoot`, the `ExceptionRoot` field, its `Clear()` line, and the
   mention in the `SerializeCache` comment.
4. `Sema/Private/Layout/TypeStoreSerialize.cpp`: drop the two `ExceptionRoot`
   lines. `ByNodeKind` is already serialised, so the claim survives the cache
   with no new field.
5. Replace the five `GetExceptionRoot()` call sites with
   `LookupNodeKind( "RaiseExpr" )`.
6. **New behaviour (Ruby-strict):** in `ExprInferencer`'s `RaiseExpr` arm, once
   the exception expression is inferred, refuse it unless its nominal descends
   from the claimer — `IsSubclassOf( Types, Nominal, *Root )`, the predicate the
   rescue-filter check at `:706` already uses. Diagnostic wording to mirror
   Ruby's: *exception class/object expected*.
7. Bump `FrontendCacheMagic` (TypeStore loses a serialised field).

Gate: clean `-Werror` build, `All CTest` no worse than the 320/334 baseline,
plus a new negative sample for `raise 42`.

### Phase B — `@[Unhandled]` → prelude wrapper

Infrastructure needed, none of which exists today:

- **A prelude file.** `LoadStdLib` (`Driver.cpp:477`) already walks
  `source/Lib/**` and prepends it to every build, so a new `.vl` there is
  compiled into every program with no new mechanism.
- **A Volt-callable handle on the unit inits.** Top-level statements compile
  into `_V_init_N` (`LlvmEmitter.cpp:575`), called from the C shell. The
  backend emits one `_V_init_all` calling each in order and checking the tag
  between them; the prelude declares it
  `@[External( "volt", "_V_init_all" )]` — inside the closed list, and it
  states a linker symbol, which is exactly what `@[External]` is for.
- **The entry function's name as a build option**, not a hardcode:
  `EmitOptions::EntryFunction` alongside the existing
  `EmitOptions::EntrySymbol = "main"` (`LlvmEmitter.hpp:58`). A configured
  spelling in the same category as the C entry symbol, which is already a
  string in C++.

The prelude then reads, in Volt:

```volt
def __volt_entry -> Int32
  begin
    __volt_run_units()
    0
  rescue e : Exception
    e.report_unhandled()
    1
  end
end
```

`report_unhandled` loses `@[Unhandled]` and becomes an ordinary method — the
compiler never learns it exists. `EmitEntryPoint` (`LlvmEmitter.cpp:499`)
shrinks to: call the entry function, return its `i32`. `UnhandledHook`,
`Member::bUnhandled` and its `TypeBinder` plumbing are deleted.

Under Ruby-strict, the catch-all second `rescue` from the original sketch is
unnecessary: nothing that is not an `Exception` can be in flight.

Gate: same as phase A, plus the uncaught-exception exit status and stderr text
unchanged on the existing samples.

### Phase C — close out

- The guardrail must print exactly `@[External  @[Literal  @[Primitive`:
  ```sh
  grep -RhoE '@\[[A-Za-z]+' source/Lib | sort -u
  ```
- Rewrite the *Remaining debt* section of `rules/zero-hardcode.md` as a second
  worked example next to `@[Apply]`, and update
  `.agents/backend/llvm.md`'s exception section.
- `format`, then `graphify update .`, then `tidy` once (end of epic).

## 6. Open risks

- **Phase B changes when unit inits run relative to the C shell.** Anything
  assuming `main` calls `_V_init_N` directly (the stdlib artifact build,
  `StdlibArtifact.cpp:112`, sets `EntrySymbol` empty) must be re-checked.
- **`__volt_entry` must not be dead-stripped** from the stdlib artifact: it is
  referenced only from the emitted shell of a *different* module.
- The prelude's `begin/rescue` is the first place a `rescue` wraps a call into
  compiler-emitted code. If the tag check after an `@[External]` call turns out
  to be elided anywhere, phase B stalls and the fallback is to keep the C shell
  reading the tag but call an ordinary (un-annotated) method found through the
  root's node-kind claim.

## 7. LIVE STATUS — mid-Phase-A checkpoint (context ran low, resume here)

Session context is about to be compacted/lost. This section is the
resume-from-here record for Phase A. Update it as you go; delete this section
only once Phase A's gate (step 7 below, plus tests) is fully green.

### Done so far (verified edits, not yet built/tested)

1. `source/Lib/Primitives/Exception.vl`: `@[ExceptionRoot]` → `@[Literal( RaiseExpr )]`
   on `class Exception`. DONE.
2. `source/Volt/Sema/Private/Layout/TypeBinder.cpp`: deleted the
   `else if ( AnnoName == "ExceptionRoot" )` branch (was ~line 598-608, right
   after the `Literal` branch closes). DONE. The `Literal` branch above it
   already handles `RaiseExpr` via `Store.BindNodeKind` — no other change
   needed there.

   NOTE: after this edit, the IDE reported clangd diagnostics on
   `TypeBinder.cpp` lines 48-71 ("unknown type name 'Symbol'", "undeclared
   identifier 'Frontend'/'Core'", a `std::is_same` template error on
   `PendingAnnotation`). These line numbers are **before** the edited region
   (~598) and the errors look like a stale/desynced clangd index (undeclared
   `Frontend`/`Core` namespaces make no sense for a file that obviously
   compiled before this three-line deletion) rather than a real regression —
   but this was **not confirmed**. First step on resume: re-open
   `TypeBinder.cpp` in the IDE / re-run clangd, or just do the full build
   (step below) and see if it's a real compile error or just stale
   diagnostics. Do not assume either way.

### Not yet done (Phase A remaining)

3. `source/Volt/Sema/Public/Volt/Sema/Layout/TypeStore.hpp` — delete:
   - `SetExceptionRoot` method (~line 507-515)
   - `GetExceptionRoot` method (~line 517-520)
   - the `// --- Exception root ---` comment block above them (~502-506)
   - the `ExceptionRoot = NominalId{};` line inside `Clear()` (~line 611)
   - the `NominalId ExceptionRoot;` private field (~line 642)
   - the mention of "ExceptionRoot" in the `SerializeCache` comment (~line 581)
   - also check the comment at line ~108-115 on `Member::bUnhandled` which
     says "Declared on the `@[ExceptionRoot]` type" — update wording to
     "the type claiming `@[Literal( RaiseExpr )]`" (cosmetic, but keep it
     accurate; this member itself (`bUnhandled`) stays for Phase A, only
     removed in Phase B).
4. `source/Volt/Sema/Private/Layout/TypeStoreSerialize.cpp` — delete:
   - `Meta::Serialize( W, ExceptionRoot );` (line 26)
   - the `if ( not Meta::Deserialize( R, ExceptionRoot ) )` block (line 64)
   - `ByNodeKind` is already serialised elsewhere in this file, so the
     `RaiseExpr` claim survives the cache with no new field — verify this by
     reading the file before editing (it was not opened yet this session).
5. Replace the 5 `GetExceptionRoot()` call sites with
   `Types.LookupNodeKind( "RaiseExpr" )` (returns the same
   `std::optional<NominalId>` shape, so call sites should need no other
   change):
   - `source/Volt/Backend/BackendLLVM/Private/ExceptionEmitter.cpp:103`
     (`ExceptionStorageSlot`, `Store.GetExceptionRoot()`)
   - `source/Volt/Backend/BackendLLVM/Private/ExceptionEmitter.cpp:136`
     (`UnhandledHook`, `Store.GetExceptionRoot()`)
   - `source/Volt/Sema/Private/Passes/TypeChecker/ExprInferencer.cpp:254`
     (`MakeExceptionConstructor`, `Context.Ctx.Types.GetExceptionRoot()`) —
     also update the error message at line 257 from
     `"no type is annotated @[ExceptionRoot]; ..."` to something like
     `"no type claims @[Literal( RaiseExpr )]; the stdlib must declare one"`.
   - `source/Volt/Sema/Private/Passes/TypeChecker/ExprInferencer.cpp:704`
     (rescue-clause filter check, `Context.Ctx.Types.GetExceptionRoot()`)
   - `source/Volt/Sema/Private/Passes/TypeChecker/ExprInferencer.cpp:713`
     (bare-rescue default filter, same)
   - Also sweep comments mentioning `@[ExceptionRoot]` for accuracy (not
     load-bearing, but `rules/zero-hardcode.md`-adjacent files like this one
     expect comments to match reality): `ExceptionEmitter.cpp:97,328`,
     `ExprInferencer.cpp:244,673`, `LlvmState.hpp:204,569,574`,
     `LlvmEmitter.cpp:542`, `ParseExpr.cpp:378`, `TypeStore.hpp:110`. Do this
     sweep last, after the functional edits, in one pass.
6. **New behaviour (Ruby-strict), in
   `ExprInferencer.cpp`'s `RaiseExpr` arm** (currently lines ~642-684 in the
   pre-edit file — re-check line numbers after edits above shift them):
   after `InferExpr( Context, Exception )` is called (line ~681) and the
   `Exception` expr's type is known, refuse it unless its nominal descends
   from the root type found via `LookupNodeKind( "RaiseExpr" )`, using the
   existing predicate `IsSubclassOf( Context.Ctx.Types, Nominal, *Root )`
   (declared `source/Volt/Sema/Public/Volt/Sema/Layout/TypeResolve.hpp:246`,
   defined `TypeResolve.cpp:141` — same predicate the rescue-filter check at
   the old line 706 already uses). Get `Nominal` the same way line 702-703
   does: `Context.Ctx.Values.Has( Type ) ? Context.Ctx.Values.Get( Type ).Base
   : NominalId{}`. Diagnostic wording to mirror Ruby's `is_a?(Exception)`
   refusal — something like: `"exception class/object expected"` at `Loc`.
   Skip the check when `Root` has no value (nothing declared a root — the
   `MakeExceptionConstructor` fallback already reports that case) or when
   `Nominal` is invalid (some other error already fired for this expression).
7. Bump `FrontendCacheMagic` — **not yet located this session**. Grep for it
   (`grep -rn "FrontendCacheMagic" source/Volt`) — likely near
   `TypeStoreSerialize.cpp` / the frontend cache header
   (`rules` mention "Issue #61" stdlib cache, see memory
   `volt-issue-61-stdlib-cache` / `.agents/PROGRESS-issue-61.md`). TypeStore
   loses a serialised field (`ExceptionRoot`), so the magic must change or a
   stale on-disk cache silently deserialises garbage.
8. Add a new negative sample exercising `raise 42` (Ruby-strict refusal) —
   place it near existing exception samples (search `samples/` for existing
   `Exception`/`raise`/`rescue` fixtures to match convention and location).
9. Gate: clean `-Werror` build through the IDE (never a bare module run —
   see `.claude/CLAUDE.md` / memory `feedback-no-run-module-config`), then
   **All CTest** IDE configuration (memory `feedback-tests-via-clion`), no
   worse than the 320/334 baseline mentioned in the plan, plus the new
   `raise 42` negative sample passing.

### Explicitly NOT started yet
Phase B (`@[Unhandled]` → prelude wrapper) and Phase C (close-out: guardrail
grep, docs, format, graphify update, tidy) — see sections 4/5 above in this
same file. Do not start those until Phase A's gate is green.

### Tool/workflow reminders for whoever resumes
- Use CLion MCP tools (`mcp__clion__*`) and Graphify
  (`graphify-out/GRAPH_REPORT.md`, `graphify query`) per user instruction —
  this task was explicitly asked to use IDE context + Graphify.
- Build/format/tidy only through IDE run configurations, per
  `.agents/rules/cpp-style.md` and `.claude/CLAUDE.md`'s "Two Reflexes".
- Do not commit — memory `no-autonomous-commits` — leave the working tree for
  the user to review and commit.
- TaskCreate/TaskUpdate task IDs from this session (may or may not survive
  compaction): #1 Phase A (in_progress), #2 Build & test Phase A (pending),
  #3 Phase B (pending), #4 Phase C (pending).
