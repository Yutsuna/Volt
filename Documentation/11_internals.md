# Compiler and VM Internals

This section provides a deep-dive specification of the Volt compilation model, virtual machine architecture, register execution mechanics, and memory safety invariants.

---

## 1. Instruction Layout (32-Bit Fixed Width)

The Volt Tier-0 virtual machine is a register-based execution engine. Instructions are represented as 32-bit packed integers to maximize cache friendliness and instruction decoding speeds. Instructions follow two primary layouts:

### Layout Formats

```
Form ABC:
┌───────────┬───────────┬───────────┬───────────┐
│ Op (8b)   │ Reg A(8b) │ Reg B(8b) │ Reg C(8b) │
└───────────┴───────────┴───────────┴───────────┘

Form ABx:
┌───────────┬───────────┬───────────────────────┐
│ Op (8b)   │ Reg A(8b) │       Val Bx (16b)    │
└───────────┴───────────┴───────────────────────┘
```

*   **Op (8 bits)**: Specifies the instruction opcode.
*   **Reg A (8 bits)**: Typically represents the destination register index.
*   **Reg B (8 bits)**: Typically represents the first operand register index.
*   **Reg C (8 bits)**: Typically represents the second operand register index.
*   **Val Bx (16 bits)**: An unsigned or signed immediate value or constant pool index.

---

## 2. Core Opcode Specifications

Volt VM relies on specialized opcodes to implement its features, arithmetic operations, dynamic dispatch, and memory management.

| Opcode | Format | Description |
| :--- | :--- | :--- |
| `LOAD_CONST` | ABx | Loads a constant value from the chunk's constant table into Register A. |
| `MOVE` | ABC | Copies the value from Register B to Register A. |
| `ADD_INT` | ABC | Performs integer addition: `Reg A = Reg B + Reg C`. |
| `ADD_F64` | ABC | Performs float addition: `Reg A = Reg B + Reg C`. |
| `INIT_OBJ` | ABx | Allocates a zero-initialized block of heap memory of size Bx, writes the class TypeID, and stores the pointer in Register A. |
| `COPY_BLOCK` | ABC | Copies a memory block of size Reg C from source pointer Reg B to target pointer Reg A (used for struct copy-by-value). |
| `CALL_METHOD` | ABC | Dispatches a dynamic class method call. Register A contains the object reference, Register B contains the VTable index offset. |
| `CALL_MIXIN` | ABC | Dispatches a mixin method. Locates the mixin vtable by traversing the host object's ITable, and jumps to the method offset. |
| `DROP` | ABC | Evaluates the object reference in Register A. Calls `finalize` if present, invokes `__drop_fields`, and frees the memory. |
| `DROP_SCOPE` | ABx | Releases all active class references declared within the lexical scope block index Bx. |

---

## 3. Execution Frames and Register Allocation

Unlike stack-based virtual machines (like JVM) that push and pop operands, the Volt VM uses **Register Windows** associated with call frames.

```
       [ Global Constants Table ]
                   │
                   ▼
  ┌─────────────────────────────────┐
  │ Frame 1 (Main)                  │
  │ Registers: R0 ... R255          │
  └────────────────┬────────────────┘
                   │  (CALL_METHOD)
                   ▼
  ┌─────────────────────────────────┐
  │ Frame 2 (Function)              │
  │ Registers: R0 ... R255          │
  └─────────────────────────────────┘
```

*   **Registers Window**: Each VM call frame contains its own array of 256 virtual registers (`R0` through `R255`).
*   **Register Mapping**: Local variables and temporary compiler calculations are mapped to unique registers by the bytecode compiler's register allocator (`FnEmitter`) at compile time.
*   **No Stack Overflow**: Operand boundaries are checked at compile time, eliminating standard runtime stack manipulation overhead.

---

## 4. Compile-Time Memory Optimizations

Although Volt features automatic reference dropping, it optimizes allocation paths at compile time using AST-based optimizations.

### Escape Analysis Heuristics
The compiler runs an escape analysis pass before generating bytecode:
1.  **Heap Allocation**: If a class instance or struct address is stored in a global variable, returned from a function, or captured by a closure, it is marked as **escaping** and allocated on the heap (`INIT_OBJ`).
2.  **Stack Promotion**: If a class instance is constructed, utilized, and goes out of scope entirely within the same function without escaping, the compiler optimizes the allocation by promoting the object to the function stack, eliminating heap allocation and garbage collection overhead.

---

## 5. Scope Unwinding and Drop Maps

To guarantee memory safety when exceptions occur or when exiting functions early via `return` or `break`, the compiler tracks scopes and generates drop maps.

### Lexical Scope Tracker (`ScopeStack`)
During bytecode emission, the compiler tracks active local variables. When a control-flow jump (such as `return` or `break`) escapes a scope, the compiler:
1.  Consults the `ScopeStack` to find all active class instances.
2.  Inserts explicit `DROP` opcodes for each active reference directly before the jump instruction.

### Unwinding and Exception Safety
If a runtime error occurs, the VM unwinds the call stack:
*   The VM scans the chunk's **Drop Map** to identify active references for the instruction pointer (IP) where the error was thrown.
*   The VM executes the corresponding `DROP` routines for those active references in reverse allocation order before reclaiming the frame, preventing memory leaks during crashes.
