# Rule: value AST — arena + typed Ids, never smart pointers

The AST is **flat value storage**, not a pointer graph. This keeps an
`AstContext` copyable, cache-friendly, trivially serialisable (hot-reload) and —
crucially — **one context per file**, so parse + sema are fully parallel.

- Every category has its own arena in `AstContext`: `Arena<ExprNode, ExprId>`,
  `Arena<StmtNode, StmtId>`, `Arena<DeclNode, DeclId>`, `Arena<TypeNode, TypeId>`,
  `Arena<Param, ParamId>`.
- References between nodes are **strongly-typed `Id`s** (`Core::TypedId<Tag>`,
  a u32 handle), never `T*` or `unique_ptr<T>`. `ExprId` and `StmtId` are
  distinct types and never interchangeable.
- Child lists are `NodeList<T> = SmallVec<T, 4>` — inline capacity for the common
  small case, no heap for most nodes.
- Create with `Context.Add(Node{...})` → returns an `Id`. Read with
  `Context.Expr(Id)` / `Stmt(Id)` / `Decl(Id)` / `Type(Id)`.
- A pass rewrites a node **in place** by assigning back into its slot; because
  parents refer to the `Id`, the whole tree updates at once and order is
  irrelevant (see `JsxLowering`).

Never introduce `std::shared_ptr`/`unique_ptr`/raw owning pointers into AST
nodes. Cross-file data (module names in the circuit graph) is plain `std::string`,
not interned symbols, since symbols are per-file.
