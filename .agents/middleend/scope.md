# MiddleEnd Specification: Scope Resolution (`ScopeResolver`)

The `ScopeResolver` pass (`source/Volt/Sema/Private/Passes/ScopeResolver.cpp`, Pass Order 10) builds lexical scope hierarchies and performs identifier binding prior to type checking.

---

## The Scope Hierarchy (`ScopeTable`)

`ScopeResolver` walks the AST and constructs a `Sema::ScopeTable` (`source/Volt/Sema/Public/Volt/Sema/Scope/ScopeTable.hpp`).

### Scope Categories (`EScopeKind`)
- **`Global`**: Top-level file namespace scope.
- **`Module`**: Module namespace scope containing free functions and module constants.
- **`Nominal`**: Class, struct, or interface scope containing fields and member methods.
- **`Method`**: Function or method body scope containing parameters and return types.
- **`Block`**: Lexical block scope (`{ ... }`).
- **`Branch`**: Control-flow branch scope (`if`, `when`, `while`).

```
 GlobalScope
  └── ModuleScope ("Math")
       └── NominalScope ("Vector")
            └── MethodScope ("normalize")
                 ├── LocalDecl ("mag")
                 └── BlockScope
                      └── LocalDecl ("temp")
```

---

## Symbol Declaration & Resolution Rules

### 1. Single-Scope Duplicate Rejection
Redeclaring a variable or parameter within the **exact same lexical scope** is illegal and produces an immediate diagnostic error (`RedeclareSameScope`):

```volt
let x = 10
let x = 20  # ERROR: Redeclaration of 'x' in the same scope
```

### 2. Shadowing Allowance
Declaring a variable in a **child scope** with the same identifier as a parent scope variable is explicitly permitted:

```volt
let x = 10
if condition {
    let x = "shadowed"  # OK: Child scope shadowing
}
```

### 3. Static Context Enforcement (`bStaticContext`)
When resolving an identifier inside a static method or module function:
- Referencing `self` or instance member variables (`@field`) triggers a semantic failure (`CannotReferenceInstanceInStaticContext`).

---

## Closure Analysis & Capture Synthesis

When `ScopeResolver` encounters a `Lambda` node (`&(x) => x + captured`), it computes variable captures and synthesizes closure environment frames.

### Capture Detection Algorithm
1. The resolver tracks variable lookups against the current `MethodScope` hierarchy.
2. If an identifier resolves to a variable declared in an outer enclosing `MethodScope`, it is marked as a **Capture**.
3. Captures are registered in `ClosureFrame` along with field offsets computed by `LayoutEngine`.

### Escape Analysis (`bEscapes`)
`ScopeResolver` performs escape analysis to optimize closure memory allocation:
- **`bEscapes == false` (Non-Escaping Closure)**: The closure is passed directly to an immediate call site (e.g., `list.map(&(x) => x * 2)`). The closure environment frame is allocated on the stack (via `alloca` in LLVM IR / stack frame in VM).
- **`bEscapes == true` (Escaping Closure)**: The closure is returned from a function or stored in a data structure. The closure environment frame is heap-allocated through the standard library allocator.

```cpp
struct ClosureFrame
{
    std::vector<CaptureField> Fields;
    std::size_t TotalSize = 0;
    std::size_t Alignment = 1;
    bool bEscapes = false; // Controls stack vs heap allocation
};
```

## What consumes this table

`ScopeResolver`'s output here is purely lexical (no `TypeStore`/cross-file
lookup), which is what makes it safe to reuse well after this pass runs.
`ClosureLifting` (`Sema/.../TypeChecker/ClosureLifting.cpp`, inside
`TypeChecker`, after every constraint in the unit has settled) is the actual
consumer: it rewrites every `Lambda`/`Block` literal into a synthesized
top-level function plus an ordinary `Proc.new( FuncAddr, env )` construction
at the literal's own site, using this table's `Fields`/`TotalSize` to lay out
a heap-allocated `Pointer<UInt8>` buffer and rewrite each captured-variable
use into an ordinary `Deref`/`Call` load. `Lambda`/`Block` are sugar
(`rules/core-ast.md`) precisely because of this — neither node survives past
`TypeChecker`, so no backend ever sees a `ClosureFrame` or binds a capture
itself.
