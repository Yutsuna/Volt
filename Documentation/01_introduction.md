# Introduction

Volt is a general-purpose, high-performance, gradually typed programming language designed with an emphasis on developer ergonomics, type safety, and predictable execution dynamics. Inspired by the expressive syntax of Ruby and Crystal, Volt delivers a modern system-level scripting experience without the overhead of a garbage collector.

---

## Language Philosophy

Volt is built upon three core design pillars:

1.  **Garbage Collector Free Execution**: Volt completely eliminates stop-the-world latency pauses, allocation tracking, and write-barriers at runtime. Memory safety and reclamation are managed entirely via static lifetime analysis at compile time, coupled with deterministic RAII (Resource Acquisition Is Initialization) destruction.
2.  **Gradual and Inferred Typing**: The language supports type inference by default, enabling rapid scripting and prototyping. Developers can gradually introduce explicit type annotations to enforce contracts, optimize compile-time layouts, and improve IDE completion.
3.  **Unified Frontend-Backend Architecture**: With native compilation, WebAssembly targeting, and a built-in JSX-like component engine, Volt is equally suited for systems programming, command-line utilities, and rich web interfaces.

---

## Core Architecture and Execution Path

The Volt compiler and runtime pipeline is divided into clear phases:

```
[ Source Files (.vl, .vlx) ]
             │
             ▼
      [ Lexer / Scanner ]
             │
             ▼
     [ AST / Parser ]
             │
             ▼
[ Semantic & Type Analysis ] ──► (Validates types, builds VTables / ITables)
             │
             ▼
   [ Typed AST Generator ]
             │
             ▼
    [ Bytecode Compiler ] ───► (Emits chunks and registers layout)
             │
             ▼
    [ VM (Tier-0 Interpreter) ] ◄──► [ JIT (Tier-1 Cranelift) ]
```

1.  **Lexing & Parsing**: Textual source is transformed into tokens and parsed using a modular, recursive-descent parser with precedence climbing.
2.  **Semantic Analysis**: The compiler performs scope check, namespace collection, type inference, and VTable/ITable resolution. Errors are raised with exact line, column, and file diagnostics.
3.  **Bytecode Compilation**: The typed AST is lowered into a register-based bytecode representation. Functions are compiled into independent executable units called Chunks.
4.  **Tiered Execution Engine**:
    *   **Tier-0**: A direct-threaded register-based virtual machine executing 32-bit packed instructions.
    *   **Tier-1**: A JIT compiler converting bytecode chunks directly to native machine code via Cranelift, monitored by hotness counters.

---

## Memory Management Model

Volt avoids runtime garbage collection by using deterministic memory management (RAII). 

*   **Value Types (Structs)**: Allocated inline on the VM execution stack. They have value semantics and are copied byte-for-byte upon assignment or function transit.
*   **Reference Types (Classes)**: Allocated on the heap. Reference variables hold pointers to the heap memory. Their allocation is tracked lexically, and the compiler automatically emits destruction instructions (`DROP`) when references exit their active scopes.

This model guarantees that system resources (memory, file handles, network sockets) are freed the exact moment they are no longer needed, maintaining a minimal and predictable memory footprint.
