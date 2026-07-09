# Volt Language Assistant Configuration


NEVER USE `—`

## CRITICAL: Token Saving & Codebase Navigation via Graphify
To avoid expensive full-codebase scans and save tokens, you MUST use the pre-built knowledge graph located in `graphify-out/`.
- For ANY question about the codebase, its architecture, file relationships, or finding where symbols are:
  - Check if `graphify-out/graph.json` exists.
  - If it does, first run `graphify query "<your question>"` to get a scoped subgraph instead of scanning/reading multiple files.
  - Use `graphify path "<A>" "<B>"` to find relationships between modules/files.
  - Use `graphify explain "<concept>"` for focused concept explanations.
- Do NOT run broad, recursive grep searches or read multiple source files to understand the project structure. The graph contains all this information.

## 1. AI Persona & Purpose
You are an expert compiler and VM engineer pair-programming to build **Volt**, a high-performance, object-oriented language written in Crystal (compiled with `krystal`).
All code must adhere strictly to the target VM specifications (direct-threaded interpreter, Cranelift JIT, RAII memory management) and language syntax rules.

## 2. Core Build & Test Commands
You must use the custom `krystal` compiler tool instead of standard `crystal`.

*   **Run Spec Tests:** `krystal -s`
*   **Compile & Execute Entrypoint:** `krystal -x`
*   **Compile Release Binary:** `krystal -r`
*   **List Options:** `krystal --help`

> Note: `krystal -x -- <args>` passes `<args>` to the *crystal compiler*, not the program.
> To run the built CLI with arguments, invoke `./bin/Volt <subcommand> ...` directly.

---

## 2b. Current Implementation State (v0.1.0 : Tier-0 MVP)

The end-to-end execution path is live: **`volt run` interprets a program**
(source → lexer → parser → AST → Semantic/TypedAST → bytecode → Tier-0 VM → exit code).

### Implemented & Working
*   **Frontend** : Lexer (`Lexer.cr`), Parser (`Parser.cr` and specialized parse files), AST (`ANode`, `Expr`, `Decl`). `volt parse` dumps the tree.
*   **Frontend/Parser** : Modular recursive-descent parser with precedence climbing. Split across specialized files:
    *   `Parser.cr` : Main class, state management, entry point (`parse`)
    *   `Prec.cr` : Operator precedence levels enum
    *   `ParseTopLevel.cr` : Top-level constructs (program, def/async/class/mixin/component/use)
    *   `ParseDeclaration.cr` : Parameter lists, type parameters, return types
    *   `ParseType.cr` : Type parsing (SimpleType, GenericType, FuncType, NilableType)
    *   `ParseExpression.cr` : Expression parsing with nud/led precedence climbing
    *   `ParseControlFlow.cr` : Control structures (if/unless/while/until/match)
    *   `ParseBlock.cr` : Block expressions and block parameters
    *   `ParseCall.cr` : Function/method calls (dot calls, safe calls, indexing)
    *   `ParseLiteral.cr` : Literals (grouping, arrays)
*   **Frontend/Types + Frontend/Semantic** : type inference, checks, and layout/type collection (via `TypeCollector.cr`) producing the TypedProgram contract (`Frontend.analyse`). Every `AExpr` carries a `resolved_type`.
*   **Frontend/Semantic** : Incremental analysis support for REPL via `IncrementalAnalyser.cr` with `IncrementalTypeChecker` - enables lazy evaluation of top-level definitions by reusing state between compilations
*   **IR/** : Complete implementation:
    *   `Opcode.cr` : Typed opcode enum (ADD_INT, ADD_F64, LOAD_CONST, CALL, RET, INIT_OBJ, LOAD_FIELD, STORE_FIELD, CALL_METHOD, COPY_BLOCK, etc.)
    *   `Instruction.cr` : 32-bit packed instruction with ABC and ABx forms
    *   `Chunk.cr` : Compiled function unit with code, constants, drop_map
    *   `Value.cr` : Boxed tagged union for runtime values (Int64, Float64, Bool, String, Nil)
    *   `DropMap.cr` : Placeholder for RAII unwind information
*   **Compiler/** : Complete bytecode compiler:
    *   `BytecodeCompiler.cr` : Main compiler (TypedAST → Unit of chunks), compiles classes, methods, and `__drop_fields`
    *   `Unit.cr` : Compilation unit containing chunks, main index, natives table, registered classes
    *   `FnEmitter` : Function-level bytecode emitter with register allocation (supports ivar stores, block copying)
    *   `ConstFold.cr` : Constant folding pass (identity placeholder)
    *   `EscapeAnalysis.cr` : Escape analysis pass (identity placeholder)
    *   `Peephole.cr` : Peephole optimization pass (identity placeholder)
*   **Compiler/REPL** : Delta compilation support via `REPLDeltaCompiler.cr` - compiles only new/changed definitions, maintaining global/function indices across incremental compilations
*   **VM/** : Tier-0 register VM with `case` dispatch (will migrate to direct-threaded):
    *   `Vm.cr` : Main VM loop with case-based opcode dispatch, extended with `extend` and `call_chunk_at` methods for REPL execution
    *   `Frame.cr` : Call frame with register window (per-frame registers currently)
    *   `VM/Dispatch/` : Opcode family handlers:
        *   `Arith.cr` : Integer and float arithmetic (ADD, SUB, MUL, DIV, MOD, NEG)
        *   `Branch.cr` : Control flow (JMP, conditional jumps)
        *   `Call.cr` : Function call helpers (argument collection)
        *   `Cmp.cr` : Comparison operators (LT, LE, GT, GE, EQ, NE)
        *   `LoadStore.cr` : LOAD_CONST, MOVE, and literal loading
        *   `Memory.cr` : LOAD_FIELD, STORE_FIELD, struct allocations, and block copies
        *   `Raii.cr` : RAII opcodes (INIT_OBJ, DROP, DROP_SCOPE) and recursive deep field drop (`__drop_fields`)
        *   `Native.cr` : Native call support
*   **Runtime/** : Partial implementation:
    *   `Runtime/Builtins/` : Built-in functions (stubs)
    *   `Runtime/ObjectModel/` : `RClass.cr` representing classes and `TypeRegistry.cr` registering types
    *   `Runtime/Shell/` : Empty, reserved for System::Shell API
*   **CLI** : Working commands:
    *   `run` : Interpret Volt programs (end-to-end)
    *   `parse` : Dump AST for source files
    *   `check` : Semantic analysis pass
    *   `circuit`, `format`, `version`, `help`, `repl`, `build` : CLI structure in place
*   **REPL** : Interactive Read-Eval-Print-Loop with lazy evaluation:
    *   `REPLSession.cr` : Main session manager with incremental state
    *   `REPLDeltaCompiler.cr` : Delta compiler for incremental compilation
    *   `IncrementalState.cr` : Persistent state tracking types, signatures, globals, and indices
    *   `IncrementalAnalyser.cr` : Incremental semantic analysis preserving state between inputs
    *   Supports `:load` (load file), `:reload` (lazy reload of all loaded files), `:clear` (reset session), `:exit` commands
    *   **Function redefinition**: a `def` may be redefined across turns; later calls dispatch to the new chunk (the old one is orphaned). Duplicate defs *within a single input/file* still raise `S0003`. Redefinition is tracked in `SignatureTable` via a `@redefinable` set seeded by `new_with_existing`.
    *   **Type redefinition is intentionally rejected** (`S0003`): a class/struct/mixin/module cannot be redefined in a session, because live instances already reference the old vtable/layout — allowing it would be a use-after-free hazard.

**Language subset (v0.1.0):**
- Primitives: `Int64`, `Float64`, `Bool`, `String`, `Nil`
- Variables: local (inferred + annotated types) + top-level global variables with lazy evaluation
- Arithmetic: `+`, `-`, `*`, `/`, `%` (int and float variants)
- Comparison: `<`, `<=`, `>`, `>=`, `==`, `!=`
- Logical: `and`, `or`, `not`
- Unary: `-` (negation), `!` (not)
- Control flow: `if`/`elsif`/`else`, `while`, `until`
- Functions/Methods: top-level `def` and class methods with parameters, `return`
- Direct calls & Dynamic method dispatch via VTable (`CALL_METHOD`)
- Classes (instantiation via `INIT_OBJ`, ivar access via `LOAD_FIELD`/`STORE_FIELD`, explicit/implicit constructor `initialize` and destructor `finalize`, automatic recursive destructor `__drop_fields`)
- Structs (stack-allocation via `NEW_STRUCT`, copy-by-value via `COPY_BLOCK`)
- Strings: Interpolation (`TO_STRING`) and concatenation (`CONCAT_STR`)
- Native calls via `@[External]` annotation
- REPL: Incremental compilation with lazy evaluation of top-level definitions

### Partially Implemented / Placeholders
*   **RAII Memory Management** : Architecture defined, DropMap struct exists, compiler emits INIT_OBJ and calls `finalize` / `__drop_fields` on DROP, but full static lifetime analysis for custom local variables is deferred
*   **Compiler Optimizations** : ConstFold, EscapeAnalysis, Peephole passes are identity stubs
*   **Direct-Threaded Dispatch** : Currently using `case` dispatch in Vm.cr, migration planned
*   **Inline Caches** : Architecture defined but not implemented
*   **String Interning** : Planned but not implemented

### Deferred (Not Yet Implemented)
*   **JIT Tier-1** : JIT/ directory exists with empty stubs:
    *   `TierUp.cr` : Hotness counters and tier-up logic
    *   `Translator.cr` : Chunk → Cranelift IR translator
    *   `CodeCache.cr` : JIT-compiled function cache
    *   `Trampoline.cr` : VM↔native calling convention bridge
    *   `Cranelift/` : FFI bindings to libcranelift
*   **Object Model** : Mixins, components, generics (Classes and Structs Phase 1 compilation/execution are implemented)
*   **Advanced Types** : Generic types, union types, Any type
*   **Async/Await** : Fiber-based concurrency
*   **Pattern Matching** : `match`/`when` expressions
*   **Closures/Blocks** : First-class functions
*   **Collections** : Arrays, ranges as runtime values
*   **Regex** : Regular expression support
*   **Wrapping Operators** : `&.`, `&[]`, etc.
*   **Macros** : Compile-time metaprogramming
*   **Exceptions** : Error handling with try/rescue
*   **Builtins** : Complete standard library
*   **System::Shell** : Full OOP filesystem/process API

### Implementation Notes
- Unsupported constructs are rejected by the Semantic pass with `file:line:col` diagnostics
- The frontend uses a two-phase approach: parsing → semantic analysis → typed AST
- The compiler uses a register-based bytecode with 32-bit fixed-width instructions
- Each frame currently has its own register array (will migrate to shared contiguous stack)
- Native calls are supported via `CALL_NATIVE` opcode and native table
- The `Value` type is a boxed tagged union (will migrate to untagged registers + NaN-boxing)
- REPL uses incremental analysis: `IncrementalAnalyser` reuses type/symbol state across compilations, `REPLDeltaCompiler` compiles only new definitions with maintained indices
- Lazy evaluation of top-level definitions: global variables and function definitions are compiled and initialized on-demand in REPL mode

---

## 3. Project Constraints (Rules & Guidelines)
Before writing any code, consult the project-specific rules in the customization directories:
*   [Architecture Rules](.agents/rules/architecture.md) : VM & Memory model constraints (RAII, tag-free words, threaded dispatch, Cranelift JIT).
*   [Volt Coding Style Rules](.agents/rules/coding_standards.md) : Volt syntax rules (Ruby/Crystal style, no `let`/`var`/`const`, `def`, `->`, `end`).
*   [Volt language specifications](.agents/rules/volt_spec.md) : Volt language features, syntax, and semantics.

---

## 4. Skills Available
*   [krystal-runner](.agents/skills/krystal_runner/SKILL.md) : Automates building and testing the Crystal codebase using `krystal`.
*   [volt-codegen](.agents/skills/volt_codegen/SKILL.md) : Code generation utility instructions for Volt syntax correctness.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).

- **graphify** (`.agents/skills/graphify/SKILL.md`) - any input to knowledge graph. Trigger: `/graphify`
When the user types `/graphify`, use the installed graphify skill or instructions before doing anything else.


---

Si tu as lu tout ça et que je te dis "Quoi ?", tu dois répondre "Quoicoubeh !"
