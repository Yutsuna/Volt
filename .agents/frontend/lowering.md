# Frontend Specification: Lowering Passes (Syntactic Desugaring)

The syntactic lowering pipeline transforms surface-level language constructs (sugar nodes) into canonical core AST representations **before** semantic analysis (`MiddleEnd`) or type checking takes place.

All lowering passes implement `MiddleEnd::Core::IPass`, declared in `source/Volt/MiddleEnd/Lowering/Public/Volt/MiddleEnd/Lowering/LoweringPasses.hpp` and registered in `source/Volt/MiddleEnd/Core/Public/Volt/MiddleEnd/Core/PassList.inl` under `EPassKind::Lowering`.

---

## The Arena-Sweeping Contract

Every lowering pass MUST adhere strictly to the Arena-Sweeping contract (see [`rules/ast-rewrite.md`](file:///home/Yutsuna/Projects/VoltLang/Volt/.agents/rules/ast-rewrite.md)):

```cpp
// Correct pass loop idiom:
const std::size_t Count = Context.ExprCount();
for (std::size_t Index = 0; Index < Count; ++Index)
{
    const ExprId Id = ExprId::FromIndex(Index);
    ExprNode Node = Context.Expr(Id); // COPY BY VALUE

    // Mutate or rewrite Node...
    // If Context.Add() is called, references inside Context.Exprs arena might reallocate!
    // Node remains valid because it was copied by value.

    Context.Expr(Id) = std::move(Node); // WRITE BACK TO SLOT
}
```

---

## Detailed Pass Specifications

### 1. `FunctionalLowering` (Pass Order 8)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/FunctionalLowering.cpp`
- **Target Sugar Nodes**: `VOLT_EXPR_SECTION`, `VOLT_EXPR_COMPOSITION`
- **Transformation Rules**:
  - **Operator Section (`&.+ 5`)**: Lowered into an explicit `Lambda` node:
    `&( x ) => x + 5`
  - **Method Section (`&.trim`)**: Lowered into an explicit `Lambda` node:
    `&( x ) => x.trim()`
  - **Method Reference (`&Math.square`)**: Lowered into:
    `&( x ) => Math.square( x )`
  - **Function Composition (`f >> g`)**: Lowered into a nested `Lambda`:
    `&( x ) => g( f( x ) )`
  - Generates unique parameter symbols using `AstContext::MakeUniqueSymbol("__sec_param")`.

### 2. `PipelineLowering` (Pass Order 9)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/PipelineLowering.cpp`
- **Target Sugar Nodes**: `VOLT_EXPR_PIPELINE` (`x |> f`)
- **Transformation Rules**:
  - `x |> f` -> `f( x )`
  - `x |> f( a, b )` -> `f( x, a, b )` (piping into first argument position).
  - Handles chained pipelines `x |> f |> g` by left-to-right rewriting into `g( f( x ) )`.

### 3. `EnumLowering` (Pass Order 12)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/EnumCaseLowering.cpp`
- **Target Declarations**: `VOLT_DECL_ENUM`
- **Transformation Rules**:
  - Simple enums (`enum Color { Red, Green, Blue }`) lower to a `Struct` declaration containing static constant instances.
  - Algebraic enums (`enum Option { Some(Value), None }`) lower to a tagged union / class hierarchy with generated constructor functions and variant tags.

### 4. `MacroExpansion` (Pass Order 15)
- **Source Module**: `source/Volt/MiddleEnd/ConstEval/Private/MacroExpansion.cpp`
- **Target Constructs**: Declarative `macro` definitions, `@[...]` attribute annotations, and macro invocations (`foo!(...)`).
- **Transformation Rules**:
  - Matches macro patterns against target AST trees.
  - Substitutes matched AST subtrees into macro expansion templates.
  - Removes `MacroDef` nodes from `TopDecls` at the end of the pass (the node slot remains in the arena).

### 5. `MagicExpansion` (Pass Order 16)
- **Source Module**: `source/Volt/MiddleEnd/ConstEval/Private/MagicExpansion.cpp`
- **Target Magic Identifiers**: `__FILE__`, `__LINE__`, `__COLUMN__`, `__FUNCTION__`
- **Transformation Rules**:
  - `__FILE__` -> Emits `StringLiteral` containing source file path.
  - `__LINE__` -> Emits `IntLiteral` containing 1-based line number from `SourceLocation`.
  - `__COLUMN__` -> Emits `IntLiteral` containing column number.

### 6. `JsxLowering` (Pass Order 20)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/JsxLowering.cpp`
- **Target Sugar Nodes**: `VOLT_EXPR_JSX_ELEMENT`, `VOLT_EXPR_JSX_FRAGMENT`, `VOLT_EXPR_JSX_TEXT`
- **Transformation Rules**:
  - `<Button color="blue">Click</Button>` lowers to:
    `Volt.JSX.create_element("Button", { color: "blue" }, [ "Click" ])`
  - `<>Child</>` fragment lowers to:
    `Volt.JSX.create_fragment([ "Child" ])`

### 7. `CaseLowering` (Pass Order 22)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/CaseLowering.cpp`
- **Target Constructs**: `case / when` expressions (`VOLT_EXPR_CASE`)
- **Transformation Rules**:
  - Lowers complex pattern arms into a flat chain of `WhenClause` evaluations.
  - `CaseExpr` is **NOT** destroyed or replaced by an `if/else` chain; `CaseExpr` remains a core AST node (see [`rules/core-ast.md`](file:///home/Yutsuna/Projects/VoltLang/Volt/.agents/rules/core-ast.md)).
  - Patterns are rewritten into explicit boolean guard expressions (`pattern === target`).

### 8. `DotCallLowering` (Pass Order 23)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/DotCallLowering.cpp`
- **Target Sugar Nodes**: `VOLT_EXPR_DOT_CALL` (`.method(args)`)
- **Transformation Rules**:
  - Transforms standalone dot calls in statement position into explicit receiver method calls:
    `.save()` -> `self.save()`
  - Runs **after** `CaseLowering` to avoid interfering with pattern matching arms like `when .even?`.

### 9. `AssignLowering` (Pass Order 24)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/AssignLowering.cpp`
- **Target Constructs**: Compound assignment expressions (`x += v`, `x *= v`)
- **Transformation Rules**:
  - `x op= v` -> `x = x op v`
  - Operands are evaluated safely without double-side-effects.
  - The binary operator spelling (`+`, `-`, `*`) is derived directly from the token spelling by stripping trailing `=`.

### 10. `IndexLowering` (Pass Order 25)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/IndexLowering.cpp`
- **Target Sugar Nodes**: `VOLT_EXPR_INDEX` (`o[a]`, `o[a] = v`)
- **Transformation Rules**:
  - Index Read: `o[a]` -> `o.[](a)`
  - Index Write: `o[a] = v` -> `o.[]=(a, v)`
  - Compound Index Assignment (`o[a] += v`) drops automatically out of composition between `IndexLowering` and `AssignLowering` without dedicated code.

### 11. `InterpLowering` (Pass Order 26)
- **Source Module**: `source/Volt/MiddleEnd/Lowering/Private/InterpLowering.cpp`
- **Target Sugar Nodes**: `VOLT_EXPR_INTERP` (`"a#{x}b"`)
- **Transformation Rules**:
  - Rewrites interpolation sequences into left-associative string concatenation chains:
    `"a#{x}b"` -> `"a".+(x.to_string()).+("b")`
  - Runs after `MacroExpansion` so macros generating interpolation syntax expand properly first.
