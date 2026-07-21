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
  `@[Primitive("i32")]`, `@[Intrinsic("llvm.add.i32")]`, `@[External("libc","calloc")]`.
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
