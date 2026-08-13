---
name: add-sema-pass
description: Recipe to add a Volt semantic pass — one PassList.inl line plus a pure function over PassContext, auto-registered and run per file by the Driver. Use when adding scope resolution, type checking, or an AST lowering.
---

# Add a sema pass

1. **Manifest (1 line).** In
   `source/Volt/MiddleEnd/Core/Public/Volt/MiddleEnd/Core/PassList.inl`:
   ```cpp
   VOLT_PASS( MyPass, 25 )   // Order: ScopeResolver 10, JsxLowering 20, TypeChecker 30
   ```
   The registry is built from this manifest and sorted by Order.

2. **Definition.** In `source/Volt/MiddleEnd/<Submodule>/Private/` (e.g. `Analysis/Private/` or `Lowering/Private/`):
   ```cpp
   #include "Volt/MiddleEnd/Core/Pass.hpp"
   namespace Volt::MiddleEnd {
       void MyPass( PassContext& Context )
       {
           // Context.Ast   — the file's AstContext
           // Context.Types — TypeStore (name -> MemoryLayout)
           // Context.Diags — thread-local diagnostic Bag
       }
   }
   ```
   `PassRegistry()` / `RunPasses()` pick it up with no extra wiring; the Driver
   runs every pass per file across its thread pool.

3. **Walk** with `std::visit(Meta::Overloaded{ [&](Target&){…},
   [&](auto&){ /* default */ } }, Context.Ast.Expr(Id))`. Report via
   `Context.Diags` only.

   If the pass **rewrites** nodes, it sweeps the arena by index, copies the
   source node by value and assigns the slot back — it never holds an arena
   reference across an `Add()`. This is mandatory, not stylistic: the arena is a
   `std::vector`, so the lost write shows up as a rewrite that silently depends
   on file size. Read [`rules/ast-rewrite.md`](../../rules/ast-rewrite.md) and
   copy the shape from `PipelineLowering.cpp` / `JsxLowering.cpp`.

4. **Constraints:** zero-hardcode (resolve via `TypeStore`, never a Volt type
   name); no shared mutable state — the pass must be safe to run on many files at
   once.

5. **Finish:** the `format` configuration (end of phase), a clean `-Werror`
   build through the IDE; for concurrency changes, a TSAN Debug build over a
   multi-file circuit. Then `graphify update source/Volt`.
