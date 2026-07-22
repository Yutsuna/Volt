---
name: add-parse-rule
description: Recipe to add a Volt grammar rule — a Pratt parselet / binding-power row for expressions, or a recursive-descent clause for statements/decls/types. Use when supporting a new operator or syntactic form.
---

# Add a parse rule

## New operator / expression form (Pratt)

1. If the operator is a new token, add it to
   `source/Volt/Frontend/Public/Volt/Frontend/Lexer/TokenKind.inl`
   (`VOLT_TOKEN(Name, "spelling")`).
2. Add / adjust its row in `Parser/Pratt.inl` (token → binding power → parselet).
   Higher binding power binds tighter; pick left/right associativity per the
   existing rows.
3. If it produces a new node kind, use the `add-ast-node` skill first, then
   build the node in the parselet with `MakeExpr(std::move(Node), Range)`.

## New statement / decl / type clause (recursive descent)

1. Add the entry point in the relevant dispatcher (`ParseStatement` /
   `ParseDeclaration` / `ParseType`) keyed on the leading token.
2. Implement `Parser::ParseX(...)`: consume with `Expect(...)`, recover with
   `ReportHere(...)`, and guard loops so `Pos` always advances.

## Always

- Reproduce with `./build/bin/Volt <file>` before and after.
- Sweep the corpus — every `samples/**` and `source/Lib/**` file must still parse.
- `volt-build format`, clean `-Werror` build via `volt-build build`, `graphify update source/Volt`.
