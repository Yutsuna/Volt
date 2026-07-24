# Frontend Specification: Overview & Design Principles

The Volt Frontend is responsible for transforming raw UTF-8 source code into a fully desugared, structured **Value Abstract Syntax Tree (AST)**. It acts as the syntactic boundary of the compiler, isolating source-level syntax, macros, operator precedence, and syntactic sugar from semantic analysis (`Sema`) and backend code generation.

The frontend is intentionally **lazy** and **type-agnostic**: it never performs type inference, scope resolution, or identifier binding. Its sole contract is to produce an unambiguous, desugared AST ready for semantic processing.

---

## Core Design Principles

### 1. The Value AST Architecture
Volt rejects traditional object-oriented AST designs using standard smart pointers (`std::unique_ptr<Node>`) or raw heap pointers. Pointer-chasing incurs cache misses, complicates parallel pass execution, and introduces memory ownership ambiguities.

Instead, the Volt AST is stored as a set of flat, contiguous typed arenas (`Core::Arena<T, TypedId>`). All AST node pointers are replaced by 32-bit typed integer indices (`ExprId`, `StmtId`, `DeclId`, `TypeId`, `ParamId`).

- **Index Stability & Cache Locality**: Nodes within an arena are laid out sequentially in memory.
- **Arena-by-Index Pass Sweeping**: Passes traverse arenas by numerical index. Because memory reallocation during arena expansion can invalidate raw C++ references, passes copy nodes by value before mutating or appending to the arena (see [`rules/ast-rewrite.md`](file:///home/Yutsuna/Projects/VoltLang/Volt/.agents/rules/ast-rewrite.md)).
- **Parallel Context Isolation**: Each source file owns its independent `AstContext`. Parsing and initial lowering passes run in parallel without cross-thread lock contention.

### 2. The Meta-First Manifest Philosophy
Following Volt's meta-first principle (see [`rules/meta-first.md`](file:///home/Yutsuna/Projects/VoltLang/Volt/.agents/rules/meta-first.md)), AST nodes, tokens, and parser precedence tables are defined declaratively in macro manifest files (`.inl` files):

- `Nodes.inl`: Central registry for all AST node variants across `Expr`, `Stmt`, `Decl`, `Type`, and `Jsx`. Adding a new node requires a single line in `Nodes.inl` plus a reflected C++ struct.
- `TokenKind.inl`: Declarative table of all lexer tokens, keywords, and punctuation spellings.
- `Pratt.inl`: Declarative binding power and operator precedence table driving the expression parser.

Reflective C++ metaprogramming (`Meta::Reflect` and `Meta::Overloaded`) automatically generates visitors, node dumpers, AST printers, and child walkers from these manifests, eliminating boilerplates.

### 3. Complete Syntactic Desugaring (Lowering)
The frontend syntax pipeline strictly lowers all **9 syntactic sugar nodes** before semantic analysis:

| Sugar Node Category | Macro Marker in `Nodes.inl` | Lowering Pass Name | Canonical Replacement AST |
|---|---|---|---|
| String Interpolation (`Interp`) | `VOLT_EXPR_SUGAR` | `InterpLowering` (Order 26) | Left-associative string concatenation (`to_string`) |
| Index Access (`Index`) | `VOLT_EXPR_SUGAR` | `IndexLowering` (Order 25) | Method calls (`.[]` / `.[]=`) |
| Dot Call Statements (`DotCall`) | `VOLT_EXPR_SUGAR` | `DotCallLowering` (Order 23) | Explicit receiver calls (`self.method()`) |
| Section / Method Capture (`Section`) | `VOLT_EXPR_SUGAR` | `FunctionalLowering` (Order 8) | Explicit `Lambda` expressions |
| Function Composition (`Composition`) | `VOLT_EXPR_SUGAR` | `FunctionalLowering` (Order 8) | Nested `Lambda` call chains |
| Pipeline Operator (`Pipeline`) | `VOLT_EXPR_SUGAR` | `PipelineLowering` (Order 9) | Direct call application `f(x)` |
| JSX Elements / Fragments / Text | `VOLT_EXPR_SUGAR` | `JsxLowering` (Order 20) | `Volt.JSX.create_element(...)` calls |

Upon completing the frontend lowering pipeline (`volt parse --lowered`), the `AstInvariant` checker (Order 40) mechanically verifies that zero sugar nodes remain in the AST.

---

## Frontend Memory Ownership (`AstContext`)

All AST state for a single source unit is encapsulated in `Frontend::AstContext` (`source/Volt/Frontend/Public/Volt/Frontend/AST/AstContext.hpp`):

```
+-------------------------------------------------------------------+
|                        Frontend::AstContext                        |
|                                                                   |
|  +---------------------------+   +-----------------------------+  |
|  | Core::Arena<ExprNode, ...>|   | Core::Arena<StmtNode, ...>  |  |
|  +---------------------------+   +-----------------------------+  |
|  | Core::Arena<DeclNode, ...>|   | Core::Arena<TypeNode, ...>  |  |
|  +---------------------------+   +-----------------------------+  |
|  | Core::Arena<Param, ...>   |   | StringInterner (SharedRef)  |  |
|  +---------------------------+   +-----------------------------+  |
|                                                                   |
|  std::vector<DeclId> TopDecls;                                    |
|  std::vector<StmtId> TopStmts;                                    |
+-------------------------------------------------------------------+
```

Strings are interned globally into immutable 32-bit `Symbol` handles via `Core::StringInterner`. AstContext provides utility methods like `MakeUniqueSymbol(Prefix)` for generating hygienic, unique identifiers during macro and lambda expansions.

---

## Pipeline Execution Order

```
Source File (.vl)
       │
       ▼
 [1. Lexer]  ──> Token Stream
       │
       ▼
 [2. Parser] ──> Raw Value AST (Arena-backed)
       │
       ▼
 [3. Lowering Passes] (EPassKind::Lowering in Sema/PassList.inl)
       ├─ Order  8: FunctionalLowering (Sections, captures, compositions -> Lambdas)
       ├─ Order  9: PipelineLowering (x |> f -> f(x))
       ├─ Order 12: EnumLowering (enum -> struct + constants)
       ├─ Order 15: MacroExpansion (declarative macros & @[...] annotations)
       ├─ Order 16: MagicExpansion (__FILE__, __LINE__)
       ├─ Order 20: JsxLowering (<Button /> -> Volt.JSX.create_element)
       ├─ Order 22: CaseLowering (case/when -> WhenClause chain)
       ├─ Order 23: DotCallLowering (.method -> self.method())
       ├─ Order 24: AssignLowering (x op= v -> x = x op v)
       ├─ Order 25: IndexLowering (o[a] -> o.[](a))
       └─ Order 26: InterpLowering ("a#{x}b" -> string concat)
       │
       ▼
 Desugared Frontend AST (Passed to MiddleEnd ScopeResolver Order 30)
