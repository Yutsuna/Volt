---
name: add-ast-node
description: Recipe to add a new Volt AST node in ~10 lines — one manifest line plus a reflected struct, with the printer/walk falling out for free. Use when introducing a new Expr/Stmt/Decl/Type/Jsx node.
---

# Add an AST node

Goal: a new node with **no** hand-written visitor.

1. **Manifest (1 line).** In
   `source/Volt/Frontend/Public/Volt/Frontend/AST/Nodes.inl`, add under the right
   category:
   ```cpp
   VOLT_EXPR( MyThing )   // or VOLT_STMT / VOLT_DECL / VOLT_TYPE
   ```
   This generates the `*Kind` enum entry, the `*Node` variant alternative, and
   the node-name lookup.

2. **Struct.** In the matching header (`Expr.hpp` / `Stmt.hpp` / `Decl.hpp` /
   `Type.hpp` / `Jsx.hpp`):
   ```cpp
   struct MyThing
   {
       using Self = MyThing;
       Core::SourceRange Loc;
       ExprId            Operand;   // children are typed Ids / NodeList<Id>
       VOLT_FIELDS( Operand )       // or VOLT_FIELDS_NONE() if it has none
   };
   ```

3. **Arena (only if it's a new category).** Thread an `Arena<…, …Id>` + `Add` /
   getter through `AstContext.hpp`. Existing categories need nothing.

4. **Do not** edit `AstPrinter`, add a `switch`, or write a walk — `ForEachField`
   covers it. Extend `AstSelfCheck.cpp` if you want a smoke assertion.

5. **Finish:** run the `format` configuration once the node is done (end of
   phase), a clean `-Werror` build through the IDE, then
   `graphify update source/Volt`.
