# Volt Language Assistant Configuration

## 1. AI Persona & Purpose
You are an expert compiler and VM engineer pair-programming to build **Volt**, a high-performance, object-oriented language written in Crystal (compiled with `krystal`).
All code must adhere strictly to the target VM specifications (direct-threaded interpreter, CraneLift JIT, RAII memory management) and language syntax rules.

## 2. Core Build & Test Commands
You must use the custom `krystal` compiler tool instead of standard `crystal`.

*   **Run Spec Tests:** `krystal -s`
*   **Compile & Execute Entrypoint:** `krystal -x`
*   **Compile Release Binary:** `krystal -r`
*   **List Options:** `krystal --help`

> Note: `krystal -x -- <args>` passes `<args>` to the *crystal compiler*, not the program.
> To run the built CLI with arguments, invoke `./bin/Volt <subcommand> ...` directly.

---

## 2b. Current Implementation State (v1 — Tier-0 MVP)

The end-to-end execution path is live: **`volt run` interprets a program**
(source → lexer → parser → AST → Semantic/TypedAST → bytecode → Tier-0 VM → exit code).

**Implemented**
*   **Frontend** — Lexer, Parser, AST (complete). `volt ast` dumps the tree.
*   **Frontend/Parser** — Modular recursive-descent parser with precedence climbing. Split across specialized files:
    *   `Parser.cr` — Main class, state management, entry point (`parse`)
    *   `Prec.cr` — Operator precedence levels enum
    *   `ParseTopLevel.cr` — Top-level constructs (program, def/async/class/mixin/component/use)
    *   `ParseDeclaration.cr` — Parameter lists, type parameters, return types
    *   `ParseType.cr` — Type parsing (SimpleType, GenericType, FuncType, NilableType)
    *   `ParseExpression.cr` — Expression parsing with nud/led precedence climbing
    *   `ParseControlFlow.cr` — Control structures (if/unless/while/until/match)
    *   `ParseBlock.cr` — Block expressions and block parameters
    *   `ParseCall.cr` — Function/method calls (dot calls, safe calls, indexing)
    *   `ParseLiteral.cr` — Literals (grouping, arrays)
*   **Frontend/Types + Frontend/Semantic** — type inference + checks producing the
    TypedProgram contract (`Frontend.analyse`). Every `AExpr` carries a `resolved_type`.
*   **IR/** — `Value` (tagged union), typed `Opcode` enum, 32-bit packed `Instruction`,
    `Chunk`. `DropMap` is a forward-compatible placeholder.
*   **Compiler/** — `BytecodeCompiler` (TypedAST → `Unit` of chunks) with a bump register
    allocator. `ConstFold`/`Peephole`/`EscapeAnalysis` are wired as identity passes.
*   **VM/** — Tier-0 register VM with `case` dispatch; opcode families in `VM/Dispatch/*`.
*   **Runtime/** — Runtime Library
*   **CLI** — `run` and `analyse` are implemented; specs under `Spec/` (`krystal -s`).

**Language subset (v1):** primitives (Int/Float/Bool/String/Nil), local variables
(inferred + annotated), arithmetic/comparison/logical/unary operators, `if`/`elsif`/`else`,
`while`/`until`, top-level `def` functions + direct calls + `return`.

**Deferred (not yet implemented):** Cranelift JIT (`JIT/`), RAII (INIT/DROP/DropMap) +
escape analysis, inline caches, string interning, direct-threaded dispatch, classes/mixins/
components/generics, `async`, `match`, closures/blocks, ranges/arrays as runtime values,
regex, wrapping operators, macros, exceptions. Unsupported constructs are rejected by the
Semantic pass with a `file:line:col` diagnostic — never silently miscompiled.

---

## 3. Project Constraints (Rules & Guidelines)
Before writing any code, consult the project-specific rules in the customization directories:
*   [Architecture Rules](.agents/rules/architecture.md) — VM & Memory model constraints (RAII, tag-free words, threaded dispatch, Cranelift JIT).
*   [Volt Coding Style Rules](.agents/rules/coding_standards.md) — Volt syntax rules (Ruby/Crystal style, no `let`/`var`/`const`, `def`, `->`, `end`).
*   [Volt language specifications](.agents/rules/volt_spec.md) — Volt language features, syntax, and semantics.

---

## 4. Skills Available
*   [krystal-runner](.agents/skills/krystal_runner/SKILL.md) — Automates building and testing the Crystal codebase using `krystal`.
*   [volt-codegen](.agents/skills/volt_codegen/SKILL.md) — Code generation utility instructions for Volt syntax correctness.
