# Closures inside generic bodies — reinstantiation + capture sizing

Status as of this session: **3 of 4 bugs fixed and verified**; **1 bug diagnosed,
not yet fixed** (root cause pinned down precisely, fix designed, not
implemented). Next session should start by implementing the sizing fix
(section 4 below), then re-run the verification commands (section 5).

## 0. Starting point

`volt build samples/Tests/Functional/Composition.vl` failed. Isolated to a
minimal repro (`Test.vl` / `test_filter.vl` at repo root — user-created, not
committed) exercising `Array<String>#filter`/`#map`, both inherited from
`mixin Enumerable<T>` (`source/Lib/Mixins/Enumerable.vl`), both using a
capturing `do |item| ... end` block internally.

```
users = [ " alice ", "   " ]
kept = users.filter( (&.empty?.!) )
tidy = users.map( (&.trim) >> (&.prefix("usr_")) )
```

First error seen: `llvm: FuncAddr targets a Decl with no resolved
free-function entry (while emitting '_V5Array6filterI6StringE')`.

## 1. The four bugs, in the order they were hit

### Bug 1 — `LowerClosureLit`'s guard used `Has()` instead of `IsDeferred()`

**File:** `source/Volt/Sema/Private/Passes/TypeChecker/ClosureLifting.cpp`

`LowerClosureLit` (the pass that rewrites a `Lambda`/`Block` literal into
`Proc.new(FuncAddr, env)`) guarded on:
```cpp
if ( not LiteralType.IsValid() or not Context.Ctx.Values.Has( LiteralType ) )
    return;
```
`UnitTypes::Has(Id)` only checks the id is in-bounds — it does **not** mean
the type is concrete. Under a generic definition (`Enumerable<T>`'s own
body, `T` unresolved), `ExprInferencer::InferExpr` still interns *some*
`SemaType` for the literal (with an invalid/empty arg where `T` would go)
and marks the id deferred (`UnitTypes::MarkDeferred`) — `Has()` doesn't see
that. So `LowerClosureLit` fired **during the original generic-shaped pass**,
permanently rewriting the shared `do |item| ... end` node in place, with
garbage (invalid) `SemaTypeId`s baked into the synthesized closure's
`Params`/`Result`.

**Fix:** guard on `Context.Ctx.Values.IsDeferred( Id )` instead — the
documented, correct signal for "this literal's type depends on an
unresolved generic parameter" (see `UnitTypes::MarkDeferred`'s own doc
comment, and `rules/core-ast.md`'s "Generic definition bodies" section).

**Consequence:** `AstInvariant.cpp`'s `CheckSugar` (order 40, "no residual
sugar node survives Lowering") would now flag every un-instantiated generic
method's closure literal as a violation, since it legitimately stays
`Lambda`/`Block` until a concrete instantiation lowers it. Added the same
exemption `CheckTyped` already had:
```cpp
if ( Context.Values.IsDeferred( Id ) ) return;
```
right before the residual-sugar report in `CheckSugar`.

### Bug 2 — reinstantiation needs its own, non-mutating closure lowering

Once Bug 1 was fixed, the closure literal is *never* lowered by the
declaring unit's own pass. `Sema::ReinstantiateBody`
(`source/Volt/Sema/Private/Passes/TypeChecker/Reinstantiate.cpp`) must lower
it itself, once per concrete instantiation — but it **cannot** mutate the
shared AST slot the way `LowerClosureLit` normally does: the same `do
|item| ... end` node is re-walked by *every* instantiation of the same
generic method (`filter<String>`, `filter<Int32>`, …), and each needs its
own, differently-typed answer. Mutating `Ast.Expr(Id)` in place the first
time would leave every subsequent instantiation reading the first one's
(wrong) answer.

**Design:** `TypeCheckerContext` gained a `Redirects` pointer
(`std::unordered_map<std::uint32_t, Frontend::ExprId> *`, null in the
ordinary per-unit pass). `LowerClosureLit`'s five `Ast.Expr(X) = …` mutation
sites were routed through two new local `RewriteSlot` overloads:

```cpp
// existing-node overload
Frontend::ExprId RewriteSlot( TypeCheckerContext&, Frontend::ExprId Target, Frontend::ExprId Replacement )
{
    if ( Context.Redirects != nullptr ) { (*Context.Redirects)[Target.Value] = Replacement; return Replacement; }
    Context.Ctx.Ast.Expr( Target ) = Context.Ctx.Ast.Expr( Replacement );
    return Target;
}
// new-content overload — Add()s in redirect mode instead of overwriting Target
Frontend::ExprId RewriteSlot( TypeCheckerContext&, Frontend::ExprId Target, Frontend::ExprNode Content ) { ... }
```

In mutate mode (ordinary pass, `Redirects == nullptr`) behavior is byte-for-byte
identical to before. In redirect mode (`ReinstantiateBody`), `Target`'s slot
is left untouched and the substitution is recorded in the map instead — new
nodes (`Ast.Add()`) are always fine, since they're never revisited by another
instantiation.

`Sema::InstantiatedBody` (`source/Volt/Sema/Public/Volt/Sema/Layout/Instantiate.hpp`)
gained the map type and field:
```cpp
using ExprRedirectMap = std::unordered_map<std::uint32_t, Frontend::ExprId>;
struct InstantiatedBody { UnitTypes Values; UnitCallees Callees; SynthesizedFunctions Synth; ExprRedirectMap Redirects; };
```

`ReinstantiateBody` now, after its own `TrailingType(Context, MethodNode->Body)`
walk (so every reachable expr already has a concrete type in `Result.Values`,
since `Result.Values` starts empty — that's the filter condition below), sweeps
for un-lowered literals and lowers them fresh:
```cpp
const std::size_t OriginalExprCount = Ast.ExprCount();  // ast-rewrite.md: bound before Add()
for ( std::size_t ExprIndex = 0; ExprIndex < OriginalExprCount; ++ExprIndex )
{
    const Frontend::ExprId CandidateId{ ... };
    if ( not Result.Values.ExprType( CandidateId ).IsValid() ) continue;   // not reached by this walk
    if ( not holds_alternative<Lambda>(...) and not holds_alternative<Block>(...) ) continue;
    TypeCheckerPass::LowerClosureLit( Context, CandidateId );
}
```
This removed the old (broken) approach of copying `DeclUnit->Synth` entries
verbatim into `Result.Synth` with wrong-arena `SemaTypeId`s — that whole
block, and the `UnitSynth` parameter `ReinstantiateBody`/`MonoBodyEmitter`
briefly grew, is gone. `ReinstantiateBody`'s signature is back to 5 params
(dropped `const SynthesizedFunctions *UnitSynth = nullptr`).

**Backend side** (`MonoBodyEmitter.cpp`,
`source/Volt/Backend/BackendLLVM/Private/Lower/Mono/MonoBodyEmitter.cpp`):
```cpp
const Sema::InstantiatedBody Overlay = Sema::ReinstantiateBody( Store, *DeclUnit->Ast, *DeclUnit->Scopes, *Entry, Request.Owner, Request.Args );
for ( const Sema::SynthesizedFunction &SynthFn : Overlay.Synth.All() )
{
    DeclareSynthesizedFn( *Services, SynthFn, *DeclUnit, Overlay.Values );                                  // was *DeclUnit->Values (wrong arena)
    DefineSynthesizedFn( *Services, SynthFn, *DeclUnit, Overlay.Values, Overlay.Callees, &Overlay.Redirects ); // new Redirects param
}
...
Frame.Redirects = &Overlay.Redirects;   // new field on the OUTER frame too
```
`FunctionFrame` (`source/Volt/Backend/BackendLLVM/Private/Lower/FunctionFrame.hpp`)
gained `const Sema::ExprRedirectMap *Redirects = nullptr;`.
`DefineSynthesizedFn`/`DeclareSynthesizedFn`
(`source/Volt/Backend/BackendLLVM/Private/Functions/{FunctionRegistry.hpp,SynthesizedSweep.cpp}`)
— `DefineSynthesizedFn` gained a trailing `const Sema::ExprRedirectMap *Redirects = nullptr`
param, defaulted so the *ordinary* per-unit `DefineSynthesized`/`DeclareSynthesized`
sweep (concrete, non-generic closures — unaffected by any of this) doesn't
need to change its call site.

**The actual redirect consumption** — two emitter dispatch sites both read
`Ast.Expr(Id)` directly and both needed the same top-of-function check
(there are exactly two `std::visit` sites over the Expr category, per
`ExprPlaceEmitter.cpp`'s own header comment):
```cpp
if ( Frame().Redirects != nullptr )
    if ( const auto It = Frame().Redirects->find( Id.Value ); It != Frame().Redirects->end() )
        Id = It->second;
```
Added to the top of `BodyEmitter::EmitExpr` (`ExprEmitter.cpp`) **and**
`BodyEmitter::EmitAddress` (`ExprPlaceEmitter.cpp`) — missing it from
`EmitAddress` was itself a bug caught mid-session (a captured variable read
as a *place*, e.g. `block` in `block.call(item)`, goes through `EmitAddress`,
not just `EmitExpr`).

### Bug 3 — dangling `MethodNode` pointer across `Ast.Add()` (ast-rewrite.md)

`MonoBodyEmitter.cpp` fetched `MethodNode` (`std::get_if<Method>(&Ast.Decl(...))`,
a raw pointer into the Decl arena) **before** calling `ReinstantiateBody`,
then used it **after**. Bug 2's new sweep calls `Ast.Add()` on that same Decl
arena (the synthesized closure's `Method` decl) — the vector may reallocate,
and `MethodNode` silently dangles. Manifested as `MethodNode->Params.Size()`
reading a garbage huge number, then a segfault. This is exactly the hazard
`rules/ast-rewrite.md` documents, just via a different call boundary than its
own example (backend holding a Sema-arena pointer across a Sema call, not a
Sema pass holding its own reference across its own `Add()`).

**Fix:** re-fetch `MethodNode` (same `std::get_if` call) immediately after
`ReinstantiateBody` returns, before using it for anything.

### Bug 4 — `Context.SelfGenerics` used the wrong unit's generics, and never included the method's own generics

**File:** `Reinstantiate.cpp`

Once bug 2/3 were fixed, `filter<String>` compiled — but `map<U>` (has its
own method-level generic, not just `Enumerable`'s `T`) failed at codegen
with `parameter 0 of 'push' has no resolved layout`, because inside `map`'s
body, `result = Array<U>.new` never resolved `U`.

Root cause: the old code did
```cpp
if ( Owner.IsValid() and Store.Type( Owner ).Unit == Entry.Unit )
    Context.SelfGenerics = GenericsOf( Ast, Store.Type( Owner ).Decl );
```
`Owner` is the **receiver's own type** (`Array`). `Entry` (`filter`/`map`) is
declared in `Enumerable<T>`, a **different unit** (`Enumerable.vl` vs
`Array.vl`) — so the guard is false and `SelfGenerics` stays null, meaning
any `T` written inside `filter`/`map`'s own body (`Array<T>.new`,
`Array<U>.new`) can never resolve by name. This was deliberate-but-incomplete:
the comment at the site explained *why* comparing against the wrong unit's
interned symbols would be wrong, but the fix (use the *right* unit) was
never written.

**Fix:** added `DeclaringTypeOf(Store, Entry)` — scans `Store`'s NominalTypes
for the one whose `Unit == Entry.Unit` and whose `Members` contains an
own-entry with `Decl == Entry.Decl` (i.e., the type that lexically declares
`Entry`, which for `filter`/`map` is `Enumerable`, not `Array`). Then:
```cpp
Frontend::SymbolList CombinedGenerics;
// type's own generics first (matches Entry.Bindings' layout: type generics, then method's)
for ( auto Name : *GenericsOf( Ast, DeclaringTypeOf( Store, Entry ) ) ) CombinedGenerics.PushBack( Name );
// then the method's own generics (map<U> — Context.Generics() never included these before either)
for ( auto Name : MethodNode->Generics ) CombinedGenerics.PushBack( Name );
Context.SelfGenerics = &CombinedGenerics;
```
(`MethodNode` had to be fetched *before* this, not after — reordered
accordingly; this is unrelated to bug 3, which is about `MonoBodyEmitter.cpp`'s
own copy.) `ReceiverArgs`/`Context.Substitution` already carried both slots in
this exact order (`Entry.Bindings`'s own convention, unrelated to this fix),
so only the *name* list needed fixing to match.

**Verified after this fix:** `test_filter.vl` and `Test.vl` (filter + map,
including the `>>` Composition case) both **compile** successfully.

## 2. Bug 5 (not yet fixed) — closure env buffer uses a fixed 8-byte-per-field layout

Found via `valgrind` after bug 4 was fixed — `map` (and almost certainly
`filter` too, just not observably) segfaults/corrupts the heap **at
runtime**, despite compiling cleanly.

```
==xxxxx== Invalid write of size 8
==xxxxx==    at ...: _V5Array3mapI6String6StringE
==xxxxx==  Address 0x... is 0 bytes after a block of size 16 alloc'd
==xxxxx==    by ...: _V7Pointer6mallocI5UInt8E
```

LLVM IR confirms it exactly (`--emit ir`):
```llvm
%0 = call ptr @_V7Pointer6mallocI5UInt8E(i64 16)     ; env = malloc(16)  — 2 fields × 8 bytes
...
%13 = ...                                             ; env + 0
call void @llvm.memcpy...(ptr %13, ptr %result, i64 24, i1 false)   ; writes 24 BYTES at offset 0!
%17 = ...                                             ; env + 8
call void @llvm.memcpy...(ptr %17, ptr %block,  i64 16, i1 false)   ; writes 16 BYTES at offset 8!
```
`result` is `Array<String>` (`{ptr,i64,i64}` = 24 bytes), `block` is
`Proc<...>` (`{ptr,ptr}` = 16 bytes). Both captures are **aggregates wider
than 8 bytes**, but `SynthesizeClosureFrame`
(`source/Volt/Sema/Private/Layout/ClosureFrame.cpp`) hardcodes:
```cpp
constexpr std::size_t FieldSize  = 8;
constexpr std::size_t FieldAlign = 8;
```
for every captured field, regardless of its actual type. The write into
`Deref(Pointer<Field.Type>.from_address(env + offset))` correctly stores a
whole `Field.Type` value (that part of `ClosureLifting.cpp` is right) — but
the *slot* it's writing into is only 8 bytes, and the *next* field starts
only 8 bytes later, so any aggregate capture overwrites its neighbor and
(for the last field, or when the sum exceeds `TotalSize`) overflows the
`malloc`'d buffer. This corrupts the heap; the corruption is only *detected*
whenever the next `malloc`/`free` call happens to touch the clobbered
metadata (which is why `map` alone crashes immediately but `filter` alone,
tested in isolation, exits 0 without any observable symptom — that is not
evidence `filter` is fine, just that nothing after it touched the corrupted
region before the process exited).

`filter` and `to_array` (T-only, one capture, `result`) compiling and running
in isolation say nothing about correctness here either, for the same reason.

### The fix (agreed with the user): use `sizeof`, resolved by the backend, not baked into Sema as a compile-time constant

Per `rules/backend-machine-only.md`, Sema **cannot** know a type's byte size
— that's `LayoutEngine`'s job, resolved lazily by the backend once a generic
is concrete. Volt already has exactly the mechanism for this:
`Pointer<T>#malloc` in `source/Lib/Primitives/Pointer.vl` does
`libc_malloc( count * sizeof T )` — `sizeof T` is an ordinary `SizeOf` AST
node (`Frontend::SizeOf{ Type: TypeId }`), core (not sugar), and per
`core-ast.md`'s "Inert" category, its actual byte value is resolved **by the
backend**, right before it's needed, exactly matching what the user asked
for ("il faudrait que ce soit résolu juste avant le backend").

Concretely, `EmitSizeOf`
(`source/Volt/Backend/BackendLLVM/Private/Lower/Expr/ExprLiteralEmitter.cpp:340`)
does:
```cpp
const Sema::LayoutId Shape = Emitter.Types().LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Sema::BindingSite{ Id } ) );
// ... emits llvm::ConstantInt::get( Width, <actual byte size> )
```
It reads the *measured type* off `Values.SiteType(BindingSite{Id})` — **not**
off the node's own `.Type` (written-syntax) field. And Sema's own
`ExprInferencer.cpp` SizeOf arm (line ~543) writes exactly that:
```cpp
Context.Ctx.Values.SetSiteType( BindingSite{ Id }, ResolveTypeExpr( ... ) );
```
i.e. `ResolveTypeExpr` runs once, at ordinary TypeChecker time, over
whatever's written in `Expr.Type`, and the *result* (a `SemaTypeId`) is what
the backend actually reads later — the `.Type` `TypeId` field itself is
never touched again after that.

**This means a synthesized `SizeOf` node doesn't need a written `TypeId` at
all.** `ClosureLifting.cpp` already knows `Field.Type` as a `SemaTypeId`
directly (no source syntax to write) — so, exactly like `NakedTypeExpr` in
the same file already hand-stamps an `Identifier`'s type via
`Context.Ctx.Values.SetExprType(...)` to skip `ResolveTypeExpr` entirely, a
new synthesized `SizeOf` node can hand-stamp its own site type the same way:
```cpp
const Frontend::ExprId SizeOfId = Ast.Add( Frontend::ExprNode{ Frontend::SizeOf{ .Loc = Loc, .Type = {} } } );
Context.Ctx.Values.SetSiteType( BindingSite{ SizeOfId }, Field.Type );   // skip ResolveTypeExpr entirely
InferExpr( Context, SizeOfId );   // still needed: gives it the IntLiteral-family runtime int width via the ordinary SizeOf arm's OWN logic — check whether calling InferExpr re-clobbers the SiteType or is safe to call after
```
**Careful:** check `ExprInferencer.cpp`'s SizeOf arm order — it currently
*always* calls `ResolveTypeExpr` and *overwrites* `SetSiteType` unconditionally
when `InferExpr` walks the node (it doesn't check "already set"). Since
`Expr.Type` would be an *invalid* `TypeId` (we're not writing real syntax),
`ResolveTypeExpr` on an invalid `TypeId` must return `SemaTypeId{}` (check —
likely yes, `if (not Id.IsValid()) return IdType{};`, confirmed present at the
top of `ResolveTypeExpr`) which would **clobber** the hand-stamped
`Field.Type` back to invalid. **So: hand-stamp `SetSiteType` *after* calling
`InferExpr`, not before** (mirror `NakedTypeExpr`'s own ordering — it sets
`SetExprType` directly and is never passed through `InferExpr` again). Simplest
correct order:
```cpp
const Frontend::ExprId SizeOfId = Ast.Add( Frontend::ExprNode{ Frontend::SizeOf{ .Loc = Loc, .Type = {} } } );
// Give it the "always UInt64-like unconstrained literal" numeric width SizeOf
// normally gets, without going through ResolveTypeExpr at all (Expr.Type is
// intentionally never written — Field.Type is already a SemaTypeId, nothing
// to resolve from source syntax):
const auto IntLitBase = Context.Ctx.Types.LookupNodeKind( "IntLiteral" );
Context.Ctx.Values.SetExprType( SizeOfId, IntLitBase ? Context.MakeType( *IntLitBase, {} ) : SemaTypeId{} );
Context.UnconstrainedLiterals.insert( SizeOfId.Value );   // ⚠ Context here is TypeCheckerContext, not ExprInferencer's local — check field is public/reachable
Context.Ctx.Values.SetSiteType( BindingSite{ SizeOfId }, Field.Type );
```
This mirrors `ExprInferencer.cpp`'s own SizeOf arm exactly, just without ever
calling `InferExpr`/`ResolveTypeExpr` on it (since there is no written type to
resolve — avoids the clobber problem entirely, and avoids needing to reason
about calling order). Double-check `UnconstrainedLiterals` is a
`TypeCheckerContext` member reachable in `ClosureLifting.cpp`'s translation
unit (it is — same header, `TypeCheckerContext.hpp`).

Then, per field, in place of the current:
```cpp
const Frontend::ExprId OffsetId = Ast.Add( Frontend::ExprNode{
    Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( std::to_string( Field.Offset ) ) } } );
```
build a **cumulative sum of preceding fields' sizes**, each rounded up to 8
(pointer/aggregate natural alignment in this codebase — every Volt aggregate
here is pointer/int-composed, so 8-byte alignment is always sufficient; no
type-specific alignment query exists or is needed). Round-up without needing
a bitwise-AND operator (not yet verified as declared on `UInt64` — safer to
avoid): `((sz + 7) / 8) * 8` using ordinary `/` and `*` (verified available:
`Int`/`UInt64` includes `Arithmetic`, per `rules/zero-hardcode.md`).

**Where to compute this:** once, per closure, right after `Frame` is computed
in `LowerClosureLit` (both malloc-branch and non-malloc/no-capture branch is
unaffected — this only matters when `bHasCaptures`). Build:
- `FieldSizeExprs[i]` = the rounded-up `SizeOf` expr for `Frame.Fields[i].Type`
  (as above).
- `FieldOffsetExprs[i]` = `FieldOffsetExprs[i-1] + FieldSizeExprs[i-1]`
  (`Binary{Plus, ...}`, `FieldOffsetExprs[0]` is just `IntLiteral{"0"}` — offset
  zero needs no `sizeof`, avoids an always-true `+0`).
- `TotalSizeExpr` = `FieldOffsetExprs[last] + FieldSizeExprs[last]` (or fold
  during the same loop).

Then:
- Replace the single `MallocArgs.PushBack(Ast.Add(IntLiteral{TotalSize}))`
  with `MallocArgs.PushBack(TotalSizeExpr)`.
- Replace **all three** existing `IntLiteral{Field.Offset}` construction
  sites with `FieldOffsetExprs[Index]` (reprocessing the same expression `Id`
  in each of the three loops is fine and correct — `Binary`/`SizeOf` nodes
  are read-only once built, no `ast-rewrite.md` concern; only worry about not
  re-running `InferExpr` on them redundantly, which is harmless but wasteful,
  not incorrect):
  1. `EnsureBody` loop (read-back, ~current line 484-524 area — the "ensure"
     block that copies possibly-mutated captures back into the outer
     variables after the call)
  2. `OuterBody` loop (pack, ~current line 537-580 area — writes captures
     *into* env before the call)
  3. `RewriteOneCapture` (~current line ~94-125 — the *read* inside the
     synthesized closure body itself, `Pointer<CapType>.from_address(env.to_address() + Offset)`)

Given (2) and (3) both need `Field.Offset`, and (1) needs it too, computing
`FieldOffsetExprs`/`FieldSizeExprs` **once**, before all three loops run
(right after `Frame` and `Captures` are known, near where `PointerBase`/
`BytePtr` are already computed), and passing them down (or just keeping them
as a `std::vector<Frontend::ExprId>` local to `LowerClosureLit`, threaded
into `RewriteCaptureUsesStmt`/`RewriteOneCapture` as an extra parameter
parallel to `Frame`) is the natural shape. `RewriteOneCapture` currently
takes `const Sema::ClosureEnvField &Field` (which has `.Offset` as
`std::size_t`) — either add a parallel `Frontend::ExprId OffsetExpr` param,
or (cleaner) stop passing `Field` there for offset purposes and pass the
precomputed `Frontend::ExprId` directly.

**Also update `ClosureFrame.hpp`/`ClosureFrame.cpp`:** `ClosureEnvField::Offset`
and `ClosureEnvFrame::TotalSize`/`Alignment` (currently `std::size_t`) become
dead/misleading once sizing moves to `ClosureLifting.cpp`+backend. Recommend
**removing** them from `ClosureFrame.hpp` entirely (meta-first: no second,
wrong source of truth) — `SynthesizeClosureFrame` becomes purely structural
(which variables are captured, in what order, their `Site`/`Type`/`Name`),
and all byte-size concerns move to where they're actually resolved
(`ClosureLifting.cpp` building `SizeOf` nodes, `EmitSizeOf` resolving them).
Grep for `.Offset`/`.TotalSize`/`.Alignment` reads elsewhere before deleting
(`ClosureLifting.cpp` is likely the only consumer, but verify — search
`Frame.TotalSize`, `Field.Offset`, `Frame.Alignment` across
`source/Volt/Sema` and `source/Volt/Backend`).

### Open question to resolve when implementing

Does `Binary{Op: Plus}` on two `UInt64`-typed (IntLiteral-family) operands
type-check/emit correctly when *both* operands are synthesized (no source
text)? `ClosureLifting.cpp` already builds `Binary{Plus, AddrId, OffsetId}`
today (address + offset) using exactly this pattern with a plain `IntLiteral`
`OffsetId` — swapping `OffsetId` for a `SizeOf`-derived expression should work
identically as long as `SizeOf`'s own `SetExprType`/`UnconstrainedLiterals`
membership makes it type-compatible the same way `IntLiteral` is. Verify by
building and checking `--emit ir` shows a real `add` (or constant-folds) at
the malloc-size and offset sites, with the correct numeric result for a
mixed primitive/aggregate capture set.

## 3. Files touched this session (all still on disk, uncommitted)

- `source/Volt/Sema/Private/Passes/TypeChecker/ClosureLifting.cpp` — guard fix
  (bug 1), `RewriteSlot` helpers + 5 call sites (bug 2).
- `source/Volt/Sema/Private/Passes/TypeChecker/ClosureLifting.hpp` — doc
  comment updated to match.
- `source/Volt/Sema/Private/Passes/TypeChecker/TypeCheckerContext.hpp` —
  `Redirects` field added (bug 2).
- `source/Volt/Sema/Private/Passes/AstInvariant.cpp` — `IsDeferred` exemption
  in `CheckSugar` (bug 1's consequence).
- `source/Volt/Sema/Public/Volt/Sema/Layout/Instantiate.hpp` — `ExprRedirectMap`
  type + `InstantiatedBody::Redirects` field; dropped `UnitSynth` param (bug 2).
- `source/Volt/Sema/Private/Passes/TypeChecker/Reinstantiate.cpp` — dropped
  `UnitSynth` param, `Context.Redirects` wiring, new closure sweep (bug 2);
  `DeclaringTypeOf` helper + `CombinedGenerics` (bug 4).
- `source/Volt/Backend/BackendLLVM/Private/Lower/FunctionFrame.hpp` —
  `Redirects` field (bug 2); needs `#include "Volt/Sema/Layout/Instantiate.hpp"`.
- `source/Volt/Backend/BackendLLVM/Private/Lower/Expr/ExprEmitter.cpp` —
  redirect check at top of `EmitExpr` (bug 2).
- `source/Volt/Backend/BackendLLVM/Private/Lower/Expr/ExprPlaceEmitter.cpp` —
  redirect check at top of `EmitAddress` (bug 2, caught mid-session).
- `source/Volt/Backend/BackendLLVM/Private/Functions/FunctionRegistry.hpp` —
  `DefineSynthesizedFn` gained `Redirects` param; needs Instantiate.hpp include.
- `source/Volt/Backend/BackendLLVM/Private/Functions/SynthesizedSweep.cpp` —
  `DefineSynthesizedFn` sets `Frame.Redirects`.
- `source/Volt/Backend/BackendLLVM/Private/Lower/Mono/MonoBodyEmitter.cpp` —
  dropped `DeclUnit->Synth` arg, uses `Overlay.Values`/`Overlay.Callees` for
  synthesized fns (bug 2), re-fetches `MethodNode` after `ReinstantiateBody`
  (bug 3).

**Not yet touched (bug 5, still to do):**
`source/Volt/Sema/Private/Layout/ClosureFrame.cpp`,
`source/Volt/Sema/Public/Volt/Sema/Layout/ClosureFrame.hpp` (remove byte-size
fields), `source/Volt/Sema/Private/Passes/TypeChecker/ClosureLifting.cpp`
again (build `SizeOf`-based size/offset expressions instead of `IntLiteral`
constants — the file is not new to this session, just needs a second pass).

All temporary `std::fprintf(stderr, "[...debug]...")` tracing added during
diagnosis has already been removed from every file above — none should
remain. If any turns up, it's safe to delete (grep `-debug\]` across
`source/Volt` to confirm none survive).

## 4. Verification commands for the next session

Build (fast incremental, non-unity):
```sh
cd /home/Yutsuna/Volt/build && ninja
```

Repro files (already exist at repo root, untracked — do not delete):
`/home/Yutsuna/Volt/Test.vl`, `/home/Yutsuna/Volt/test_filter.vl`.

```sh
cd /home/Yutsuna/Volt
./volt build test_filter.vl -o /tmp/tf && /tmp/tf; echo "exit=$?"      # filter only
./volt build Test.vl        -o /tmp/tv && /tmp/tv; echo "exit=$?"      # filter + map (Composition)
valgrind --error-exitcode=1 -q /tmp/tv                                  # must be clean once bug 5 is fixed
```
Both currently **compile**; `Test.vl` currently **segfaults/heap-corrupts at
runtime** (bug 5). `test_filter.vl` currently exits 0 but is **not proven
correct** — re-run under valgrind once bug 5's fix lands, don't trust the
clean exit alone.

Once bug 5 is fixed, also build+run
`samples/Tests/Functional/Composition.vl` (the original failing sample) and
confirm it passes its own `assert!`s (exit 0, no `raise`).

A separate ASan build directory was started in parallel by the user
(`/home/Yutsuna/Volt/build-asan`, `meson setup build-asan -Denable_asan=true
--buildtype=debug`) — slow (no ccache). If still building, prefer it over
valgrind for the final check (matches `rules/ast-rewrite.md`'s own checklist
item 4, "an ASan build, run through the IDE, no report").

## 5. Process notes for next session

- Build via `cd build && ninja` (or the IDE's own configuration/CLion MCP
  tools) — meson is already configured, `default_library` likely shared per
  `rules/shared-lib-exports.md`; if a new cross-`.so` symbol is needed, watch
  for mold `undefined symbol` errors and add `<MODULE>_EXPORT`.
- `./volt` at repo root is a convenience symlink/copy the user maintains
  manually to `build/source/Volt/Volt/volt` — may need `cp` refresh if
  missing (`build/source/Volt/Volt/volt` is the authoritative freshly-built
  binary).
- `gdb` in this environment has broken split-DWARF (`.dwo` files, "too
  large" BFD errors) — locals/args are usually not inspectable. `valgrind`
  worked well for this session's diagnosis and is the faster tool here.
- Run `format` at the end of the phase (once bug 5 lands and everything
  passes), `tidy` only at the end of the epic — per project convention, not
  mid-phase. Several `readability`/`modernize` clang-tidy diagnostics have
  been flagping on touched files throughout this session (pre-existing
  style debt in some cases, e.g. redundant member initializers in
  `TypeCheckerContext.hpp`); ignore until the `tidy` pass.
- Do not `git add`/`git commit` — user preference (see memory
  `no-git-add-commit.md`).
