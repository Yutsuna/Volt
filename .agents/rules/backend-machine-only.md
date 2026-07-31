# Rule: backend machine-only — the backend knows scalars, pointers, and calls

Volt has three stages with three distinct vocabularies:

- **Frontend**: Lexer, AST, Parser. Types are lazy and dynamic here — nothing
  is resolved yet.
- **Middle-end**: semantics, type resolution, desugaring, lowering. This is
  where every Volt-level concept — a type, a member, a construction protocol —
  gets resolved down to something the backend can execute blindly.
- **Backend** (all 3 — LLVM, WASM, VM): declarative pattern matching only. It
  knows **exactly**: `i8 i16 i32 i64`, `u8 u16 u32 u64`, `f32 f64`, pointers,
  references, (de)referencing. Nothing else — see the identical list in
  `rules/zero-hardcode.md`'s "vocabulary the compiler is allowed".

This rule states the consequence that follows, more strictly than
`rules/zero-hardcode.md`'s literal grep for Volt type names checks for:

> **No backend may hold structural or protocol knowledge about any Volt-level
> construct, ever — not even knowledge that doesn't spell out a type name.**

A backend may read `UnitTypes`/`UnitCallees`/`MemoryLayout` and emit; it may
never *decide* anything about what a type's fields mean, how many times to
call something, or which member "the append/init/whatever protocol" is. If a
backend's node dispatch (`std::visit` over `ExprKind`) has a case that does
more than "read a resolution, emit a call, or emit a machine instruction for a
primitive/pointer receiver" (`rules/core-ast.md`'s operator contract), that
case is a bug, independent of whether any C++ identifier literally spells a
Volt type name.

**Memory allocation is never the backend's business.** It is already fully
expressible in ordinary Volt via `external def` + `@[External( "libc",
"malloc" )]` (`source/Lib/Primitives/Pointer.vl`), called like any other
resolved member. A backend calling a raw C ABI function directly (bypassing
Volt-level member resolution) is exactly as wrong as it hardcoding the
allocation size and field layout by hand — both are the backend acquiring
protocol knowledge that belongs upstream.

## Consequence: `ArrayLit`/`HashLit` must be gone before codegen

As of this writing, `rules/core-ast.md` lists `ArrayLit`/`HashLit` among the
**27 core nodes** — reasoned as "cannot be lowered, the generic element type
is only known after `TypeChecker`". That reasoning is superseded: see
`.agents/PLAN_LITERAL_LOWERING.md` for the corrected design (the middle-end
rewrites the literal into ordinary `Begin`/`Assign`/`Binary`/`Call` nodes once
its own type has stabilized, using the *same* deferred-typing convention any
other generic-body expression already uses — no new mechanism, no backend
involvement). Once that lands, `core-ast.md`'s node table, the "why core not
sugar" section, and every "27 core / 9 sugar" count across `BACKEND.md` /
`MIDDLEEND.md` / `AGENTS.md` must be updated in the same change — do not leave
the count stale the way the `If`-to-`VOLT_EXPR` move already had to correct it
once (`PLAN_LLVM.md` Phase 8).

## A synthesized operator is not a built-in either

A rewrite that needs "the append operation" (e.g. lowering `[1, 2, 3]` into a
sequence of calls) may **not** invent a special-cased notion of "the append
member" via a new annotation (`rules/zero-hardcode.md`'s closed list —
`@[Primitive]`, `@[External]`, `@[Literal]` — a fourth is refused, see
`.agents/PLAN_LLVM.md` §5c's rejected `@[LiteralAppend]`) — but it *also* may
not assume an operator symbol works structurally. `<<` is not "known" by the
compiler to mean append, or anything else, until a type declares it: exactly
as `Int32 + Int32` fails to typecheck the moment `Int32` stops including
`Arithmetic` (`+` has no meaning to the compiler beyond "whatever this type's
declared member with that spelling does"), a middle-end rewrite that
synthesizes `tmp << element` must go through the *identical* member-resolution
path (`MemberType`, `IsBuiltinOpOn`) that a hand-written `tmp << element`
would — declared with a real body in Volt (`Array<T>#<<`), non-primitive, no
backend exemption. The rewrite gets no shortcut past that resolution; if the
type hasn't declared the operator, the rewrite fails exactly the way ordinary
source would.
