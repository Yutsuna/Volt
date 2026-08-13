# Rule: shared-lib exports — annotate what crosses a `.so` boundary

Every Volt module (`Core`, `Frontend`, `MiddleEnd`, `Driver`, …) compiles with
`-fvisibility=hidden` / `-fvisibility-inlines-hidden` (`meson/meson.build`).
When `default_library=shared`, Meson builds each module as a `.so` and generates `<Module>_export.hpp` (e.g. `Core_export.hpp`) with a `<MODULE>_EXPORT` macro
(e.g. `CORE_EXPORT`). That generated header's directory is on the module's
`PUBLIC` include path, so any downstream module can `#include` it directly by
filename — no subdirectory prefix.

**Hidden visibility means a symbol not explicitly exported is invisible to
every other `.so`**, even though it links fine when linking that library on
its own — `-fuse-ld=mold`'s `--no-allow-shlib-undefined` only catches this at
the *final executable* link, so the failure shows up late, as a wall of
`undefined symbol` errors from `mold` naming things like `DiagEngine::Report`,
`Lexer::Tokenize`, `RunPasses` — not as a compile error.

## What to do

Any class or free function declared in a module's `Public/` tree that is
**called from a different module** (directly, or transitively through
`Driver`/`Volt`) must be annotated with that module's export macro:

```cpp
#include "Core_export.hpp"   // from generate_export_header(), self-module

class CORE_EXPORT DiagEngine
{
    // nested types used across the boundary need it too:
    class CORE_EXPORT Bag : public FNonCopyable { ... };
};

[[nodiscard]] CORE_EXPORT std::size_t SomeFreeFunction ( ... );
```

Place `<MODULE>_EXPORT` right after `class`/`struct`, or right before the
return type for free functions (after `[[nodiscard]]` if present).

## What *not* to annotate

Pure header-only templates (`Arena<T, IdType>`, `SmallVec<T, N>`, `TypedId<Tag>`,
`Overloaded<Fs...>`, …) never need it — every instantiation is emitted locally
in the TU that uses it, so there is no cross-`.so` symbol to resolve. Plain
value-AST aggregates (`Expr`/`Stmt`/`Decl`/`Type` node structs in
`Frontend/AST/*.hpp`) generally don't either, since they carry no out-of-line
methods — only annotate them if the linker actually asks for one.

## How to find what's missing

Don't guess from a static read of the headers — the linker is the ground
truth. Configure with `default_library=shared` and rebuild through the IDE, then
fix the exact symbols mold reports, module by module, rebuilding between
passes. This finds the precise minimal set faster and more reliably than
annotating every public declaration up front.
