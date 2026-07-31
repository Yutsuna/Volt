# Removing `@[ExceptionRoot]` and `@[Unhandled]`

The two annotations `rules/zero-hardcode.md` lists as *remaining debt*. This
file records the measured state, the design decision, and the phased plan.

Status: **implemented.** Phases A, B and C are all done and committed
(`906d101`, `53fc4dc`, `994cd14`, `4f8ca82`, plus the doc closeout below). The
guardrail in `rules/zero-hardcode.md` prints exactly `@[External  @[Literal
@[Primitive`; neither `ExceptionRoot` nor `bUnhandled` exist anywhere in
`source/Volt/` any more. This file is kept as the design record — see
`rules/zero-hardcode.md`'s "Removed: `@[ExceptionRoot]` and `@[Unhandled]`"
for the summary a future reader should start from.

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

## 7. Closed out

Phases A and B (§5) landed as `906d101` (`Prelude.vl` + `__volt_entry`),
`53fc4dc` (`bUnhandled` removed from Sema), `994cd14` (`FrontendCacheMagic`
bumped), `4f8ca82` (`EmitInitAll` / uncaught-exception handling simplified in
`BackendLLVM`). Phase C's guardrail already prints exactly `@[External
@[Literal  @[Primitive`; the remaining Phase C doc work — rewriting
`rules/zero-hardcode.md`'s debt section and `backend/llvm.md`'s exception
section, `format`, `graphify update .`, one end-of-epic `tidy` — was done in
the session that closed this file out.

Nothing here should need touching again unless the EH model itself changes
(tier 2 / Itanium, §5's "once hot" note).
