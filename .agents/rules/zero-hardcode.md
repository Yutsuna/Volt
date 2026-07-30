# Rule: zero hardcode — the compiler knows machine scalars, nothing else

## The vocabulary the compiler is allowed

Volt's C++ side understands exactly one vocabulary, and it is a *machine*
vocabulary:

- integers `i8 … i64`, `u8 … u64`
- floats `f32`, `f64`
- pointers and references

That is the entire list. `Int`, `String`, `Array`, `Int32`, `Proc`,
`Exception` are **Volt** — they live in `source/Lib/` and reach the compiler
only as a `MemoryLayout` (`Sema/Layout/MemoryLayout.hpp`):
`Primitive{ Spelling, Bits } | Pointer{ Pointee } | Aggregate{ Fields[] }`. A
primitive is an *opaque interned spelling* plus a bit width; the compiler
never learns that `"i32"` means `Int32`.

Everything the compiler decides about a value is decided **from its layout**.
Never from its name. Never from a flag an annotation set on it.

## The three annotations — a closed list

| Annotation | States | Why it cannot be derived |
|---|---|---|
| `@[Primitive( "i32", 32 )]` | which machine scalar this type *is* | nothing in `struct Int32`'s body says 32 bits |
| `@[Literal( IntLiteral )]` | which AST node kind this type wraps | the mapping node ↔ type is arbitrary and Volt's to choose |
| `@[External( "libc", "malloc" )]` | which linker symbol backs this member | the definition is outside Volt entirely |

Each states a fact **at the boundary of the language**, one the compiler has
no way to compute. None of them tells the compiler *what to do*.

**This list is closed.** A fourth annotation is not an extension point; it is
the signal that something is being modelled wrong. That is not a style
preference — every annotation is a `if ( Decl->bWhatever )` that each target
must repeat, so a rule that grows annotations grows the very per-backend
hardcode it claims to remove.

### The test, before proposing one

> Does it state what something **is**, or what the compiler should **do**?

If the answer is *do* — call this member, emit this sequence, treat this
member differently from its siblings — it is a hardcode with a Volt-side
spelling. The `if` you avoided in C++ has become an `if` on a flag, in the
same file, **plus** a stdlib edit and a serialisation field. Strictly worse
than the hardcode it replaced, because it is now spread across two languages.

Before reaching for one, check the two mechanisms that already answer most of
these questions with no new syntax at all:

- **The layout answers it.** `a + b` is a machine instruction or a method call
  depending on `LayoutKind`, never on a name (see below). This is the single
  most under-used mechanism in the codebase.
- **A node-kind claim answers it.** `@[Literal( X )]` already binds a type to
  an AST node, and `TypeStore::LookupNodeKind( "X" )` reads it back. A node
  kind is the **compiler's own** vocabulary — unlike a Volt type name — so
  naming one in C++ is not a hardcode. `TypeCompat` identifies `nil` this way
  (`LookupNodeKind( "NilLiteral" )`), `ExprInferencer` identifies a pointee
  this way (`"PointerType"`), and `MemberResolver` identifies the callable
  type this way (`"FuncType"`).

### The refused example, kept on purpose

`.agents/PLAN_LLVM.md` §5c proposed `@[LiteralAppend]` on `Array#push` and
`Hash#[]=`, so a backend could build `[ 1, 2, 3 ]` as *initialize, then one
append per element*. **Refused**, and the reasoning generalises:

- it says what to **do** (call this member, N times), not what anything *is*;
- it lands as `if ( Decl->bLiteralAppend )` in LLVM, then again in the JIT,
  then again in WASM — exactly the cost it claimed to avoid;
- **the stdlib needed nothing**: `Array#initialize` and `Array#push` already
  reach `malloc` / `free` through `Pointer<T>.malloc`
  (`source/Lib/Primitives/Pointer.vl`), which is `@[External( "libc", … )]`.
  The construction protocol was already fully expressible in Volt.

The question an aggregate literal poses is *"what does this node's layout look
like, and what fills it"* — a layout question, of the same kind
`StringLiteral` already answers. It is not *"which method should I please
call"*.

### Removed: `@[Apply]`

`Proc` used to annotate its own `call` with `@[Apply]`, so that Sema and every
backend could recognise "invoking a callable" by a flag on `Member`. It is
gone, and nothing replaced it:

- the callable type is **the type claiming the `FuncType` node kind** —
  `@[Literal( FuncType )]`, which `Proc` already carried. `IsCallableType`
  (`Sema/.../MemberResolver.cpp`) is that one lookup.
- the member invoked is that type's single `abstract` contract, found by
  walking its members — so the spelling `call` stays Volt's to choose and
  appears nowhere in C++.
- the outcome is recorded **once**, on `CalleeEntry::bIndirect`, next to
  `bConstructs`. A backend reads a resolution; it does not re-identify
  anything, and a second backend costs zero lines.

That last point is the shape to copy: when a decision genuinely needs taking,
take it in the resolver and record it on the *resolution*, not as a flag on
the *declaration* driven by an annotation.

### Remaining debt: `@[ExceptionRoot]` and `@[Unhandled]`

Both still exist (`source/Lib/Primitives/Exception.vl`, 3 sites) and both
violate this rule: `@[Unhandled]` is purely "call this member", and
`@[ExceptionRoot]` makes one type structurally special. They are listed here
so nobody mistakes them for precedent. Removing them is an exception-model
redesign — the C++ model (a type-indexed unwind table, no privileged root) is
the reference — not a deletion, so it is its own piece of work.

## Guardrails

No Volt type name may appear as an identifier in `Frontend/` or `Sema/`
(outside comments / tests):

```sh
grep -RnE '\b(String|Array|Int32|Int64|UInt8|Float64|Proc|Exception)\b' \
  source/Volt/Frontend source/Volt/Sema \
  --include='*.hpp' --include='*.cpp'
```

No annotation outside the closed list may be read anywhere:

```sh
grep -RhoE '@\[[A-Za-z]+' source/Lib | sort -u
# must print exactly: @[External  @[Literal  @[Primitive
```

If you need a "builtin", add it to the Volt stdlib and resolve it through a
layout or a node-kind claim — do not special-case it in C++, and do not invent
an annotation for it.

## Primitive operators: annotate the type once, derive the rest

`a + b` on a primitive is a machine instruction, not a Volt method body. The
stdlib still declares it, and the split of responsibility is deliberate:

```volt
@[Primitive( "i32", 32 )]
struct Int32
  include Arithmetic       # brings `abstract def +( other : self ) -> self`, ...
end

@[Primitive( "f64", 64 )]
struct Float64
  include Arithmetic       # same line, different instruction
end
```

Three distinct roles, none of which names a Volt type in C++:

1. **The abstract contract is the signature.** `mixin Arithmetic`'s
   `abstract def +( other : self ) -> self` is what gives `a + b` its type —
   `LookupMemberOn` finds it through the `include`, and `self` resolves to the
   receiver. These declarations are load-bearing; do not delete them to silence
   a conformance diagnostic.
2. **The layout is the exemption from implementing it.** A type whose layout is
   `Primitive` or `Pointer` may leave an operator contract bodyless — the
   backend supplies it. This is decided by `LayoutKind`, never by a type name,
   and the operator set itself is not hardcoded either: `IsBuiltinPrimitiveOp`
   (`Sema/.../MemberResolver.cpp`) simply accepts any name that does not start
   with a letter or `_`, plus `and` / `or` / `not`.
3. **The spelling selects the instruction** (codegen phase, see `BACKEND.md`):
   `Primitive{ Spelling, Bits }` is enough to pick `add` vs `fadd` from the
   opaque `"i32"` / `"f64"` string. The compiler never learns that `"f64"`
   means `Float64`.

Every site that reasons about "this operator is provided by the backend" must
go through the **same** predicate, or one site contradicts the other — the
unknown-member diagnostic and the abstract-conformance check both consume
`IsBuiltinOpOn( Context, Base, Name )`. A non-primitive `struct` that includes
`Arithmetic` gets no exemption and must write the bodies.

## Declaring the operator is not optional

An exemption from writing a *body* is not an exemption from writing the
*signature*. `IsBuiltinOpOn` only says "the backend supplies this"; the
declaration is still what gives `a + b` a type. Two operators were exempted and
never declared, so every expression built on them typed as unknown — silently,
because an unresolved receiver reports nothing:

- `Pointer<T>` had no `+` / `-`, so `*( buf + i )` was untyped throughout the
  stdlib. They are heterogeneous (`( offset : UInt64 ) -> Pointer<T>`), so they
  cannot come from `Arithmetic`, whose contracts are all
  `( other : self ) -> self`; they are declared on `Pointer` itself.
- `Bool` had no `and` / `or` / `not`. Those three are spelled with words, and
  the parser's operator-method table did not accept them — a `def and` produced
  a method with no name at all. `Frontend/.../ParseDecl.cpp`'s
  `IsOperatorMethodStart` and Sema's `IsOperatorName` must accept the same set;
  they are two halves of one contract.

Rule of thumb: if `IsBuiltinOpOn` would exempt an operator on a type, that type
(or a mixin it includes) must still declare it `abstract`.

## Implicit widening between scalars of the same family

`TypeCompat.cpp`'s `IsWideningScalar` accepts `Int8` where `UInt64` is
expected. This is a **language semantics decision**, not an implementation
detail, and it was forced: `mixin Hashable` contracts `hash -> UInt64` because
`Hash#[]=` computes `key.hash % @entries.capacity` against a `UInt64`, and
Volt has **no integer conversion at all** — no cast, no `to_u64`, and
`@[Intrinsic]` is recognised nowhere in `source/Volt/`. Without the rule,
`hash` is unwritable for five of the ten primitive widths.

It is deliberately narrow:

- both nominals **non generic** — the decisive guard: `Pointer<T>` is
  `@[Primitive( "ptr", 64 )]` for *every* `T`, so without it `Pointer<Int32>`
  would be assignable to `Pointer<String>`;
- **same family**, derived from the spelling (`i`/`u` integer, `f` float, `i1`
  isolated so `Bool` never widens into a number, `ptr` matched as integer);
- **never narrowing**: `Target.Bits >= Value.Bits`, so `UInt64 → Int32` stays
  an error.

Signedness deliberately does not enter the identity: `i8 → u64` is accepted —
that is the `hash` case, and a zext/sext on the backend side. Reading the
spelling is inside the vocabulary this rule grants C++ ("the spelling selects
the instruction"); no Volt type name appears. If Volt ever grows explicit
conversions, this is the first rule to reconsider.
