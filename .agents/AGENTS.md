# Volt Language Assistant Configuration


NEVER USE `—`

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
*   **Frontend** : Lexer (`Lexer.cr`), Parser (`Parser.cr` and specialized parse files), AST (`ANode`, `Expr`, `Decl`). `volt ast` dumps the tree.
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
*   **Frontend/Types + Frontend/Semantic** : type inference + checks producing the
    TypedProgram contract (`Frontend.analyse`). Every `AExpr` carries a `resolved_type`.
*   **IR/** : Complete implementation:
    *   `Opcode.cr` : Typed opcode enum (ADD_INT, ADD_F64, LOAD_CONST, CALL, RET, etc.)
    *   `Instruction.cr` : 32-bit packed instruction with ABC and ABx forms
    *   `Chunk.cr` : Compiled function unit with code, constants, drop_map
    *   `Value.cr` : Boxed tagged union for runtime values (Int64, Float64, Bool, String, Nil)
    *   `DropMap.cr` : Placeholder for RAII unwind information
*   **Compiler/** : Complete bytecode compiler:
    *   `BytecodeCompiler.cr` : Main compiler (TypedAST → Unit of chunks)
    *   `Unit.cr` : Compilation unit containing chunks, main index, natives table
    *   `FnEmitter` : Function-level bytecode emitter with register allocation
    *   `ConstFold.cr` : Constant folding pass (identity placeholder)
    *   `EscapeAnalysis.cr` : Escape analysis pass (identity placeholder)
    *   `Peephole.cr` : Peephole optimization pass (identity placeholder)
*   **VM/** : Tier-0 register VM with `case` dispatch (will migrate to direct-threaded):
    *   `Vm.cr` : Main VM loop with case-based opcode dispatch
    *   `Frame.cr` : Call frame with register window (per-frame registers currently)
    *   `VM/Dispatch/` : Opcode family handlers:
        *   `Arith.cr` : Integer and float arithmetic (ADD, SUB, MUL, DIV, MOD, NEG)
        *   `Branch.cr` : Control flow (JMP, conditional jumps)
        *   `Call.cr` : Function call helpers (argument collection)
        *   `Cmp.cr` : Comparison operators (LT, LE, GT, GE, EQ, NE)
        *   `LoadStore.cr` : LOAD_CONST, MOVE, and literal loading
        *   `Raii.cr` : RAII opcodes (INIT, DROP, DROP_SCOPE) : reserved for future
        *   `Native.cr` : Native call support
*   **Runtime/** : Partial implementation:
    *   `Runtime/Builtins/` : Built-in functions (stubs)
    *   `Runtime/ObjectModel/` : Empty, reserved for class/method system
    *   `Runtime/Shell/` : Empty, reserved for System::Shell API
*   **CLI** : Working commands:
    *   `run` : Interpret Volt programs (end-to-end)
    *   `ast` : Dump AST for source files
    *   `analyse` : Semantic analysis pass
    *   `circuit`, `format`, `version`, `help`, `repl`, `build` : CLI structure in place

**Language subset (v0.1.0):**
- Primitives: `Int64`, `Float64`, `Bool`, `String`, `Nil`
- Variables: local (inferred + annotated types)
- Arithmetic: `+`, `-`, `*`, `/`, `%` (int and float variants)
- Comparison: `<`, `<=`, `>`, `>=`, `==`, `!=`
- Logical: `and`, `or`, `not`
- Unary: `-` (negation), `!` (not)
- Control flow: `if`/`elsif`/`else`, `while`, `until`
- Functions: top-level `def` with parameters, `return`
- Direct calls (resolved at compile time)
- Native calls via `@[External]` annotation

### Partially Implemented / Placeholders
*   **RAII Memory Management** : Architecture defined, DropMap struct exists, but INIT/DROP opcodes not yet emitted by compiler
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
*   **Object Model** : Classes, mixins, components, generics
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

Si tu as lu tout ça et que je te dis "Quoi ?", tu dois répondre "Quoicoubeh !"
