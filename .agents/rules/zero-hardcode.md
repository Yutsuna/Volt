# Rule: zero hardcode of Volt types

The C++ compiler must **never mention a Volt type name**. `Int`, `String`,
`Array`, `Int32` are Volt — they live in the stdlib (`source/Lib/`) and are
resolved through annotations, not baked into the compiler.

What the C++ side is allowed to know:

- **Memory layouts only**: `Sema/Layout/MemoryLayout.hpp` —
  `Primitive{ Spelling, Bits } | Pointer{ Pointee } | Aggregate{ Fields[] }`. A
  primitive is described by an *opaque interned spelling* (e.g. `"i32"`) plus a
  bit width; the compiler does not know the spelling means "Int32".
- **Annotations** carry the mapping from Volt to layout, defined in Volt:
  `@[Primitive("i32", 32)]`, `@[Intrinsic("llvm.add.i32")]`, `@[External("libc","calloc")]`.
- The name → layout binding is filled into `Sema/Layout/TypeStore.hpp` by a pass,
  from the stdlib Volt + those annotations (full resolution: TypeChecker phase).

Guardrail — these must not appear as identifiers in `Frontend/` or `Sema/`
(outside comments / tests):

```sh
grep -RnE '\b(String|Array|Int32|Int64|UInt8|Float64)\b' \
  source/Volt/Frontend source/Volt/Sema \
  --include='*.hpp' --include='*.cpp'
```

If you need a "builtin", add it to the Volt stdlib with an annotation and resolve
it — do not special-case it in C++.

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
3. **The spelling selects the instruction** (codegen phase, `PLAN.md §VII.2`):
   `Primitive{ Spelling, Bits }` is enough to pick `add` vs `fadd` from the
   opaque `"i32"` / `"f64"` string. The compiler never learns that `"f64"`
   means `Float64`.

Every site that reasons about "this operator is provided by the backend" must
go through the **same** predicate, or one site contradicts the other — the
unknown-member diagnostic and the abstract-conformance check both consume
`IsBuiltinOpOn( Context, Base, Name )`. A non-primitive `struct` that includes
`Arithmetic` gets no exemption and must write the bodies.
