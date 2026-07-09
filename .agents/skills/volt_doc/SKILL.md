---
name: volt-doc
description: Guidelines on writing, maintaining, and verifying the technical documentation of the Volt programming language.
---

# Volt Documentation Guidelines

Activate this skill when writing, editing, or validating technical documentation for the Volt programming language.

## Documentation Standards

1.  **Technical and Direct Tone**: Avoid corporate or conversational AI cliches (e.g., "in this section, we will", "in the ever-evolving world of"). Write clear, direct, and technical descriptions inspired by Crystal and Rust documentation.
2.  **No Em Dashes**: Never emit or write the em dash (`—`) character in any document. Use simple hyphens (`-`), colons (`:`), or semi-colons (`;`) instead.
3.  **Comprehensive Explanations**: Do not write superficial summaries. Each feature must be explained with its runtime semantics, memory implications, compile-time checks, syntax rules, and precise code examples.
4.  **Syntax Accuracy**: Align examples with the actual parser grammar and tested features in the [Samples](file:///home/Yutsuna/Projects/Volt/Samples) directory.
5.  **Interactive Code Links**: Create absolute file links using the `file://` scheme when referring to project files (e.g. `[PLAN.md](file:///home/Yutsuna/Projects/Volt/Documentation/PLAN.md)`).

---

## Documentation Architecture Map

All technical files are organized under the [Documentation](file:///home/Yutsuna/Projects/Volt/Documentation) folder:

*   `01_introduction.md`: Memory model (RAII vs GC), tiered compilation, and execution pathway.
*   `02_getting_started.md`: Toolchain commands (`krystal -s`, `krystal -x`), CLI utilities, and interactive REPL behavior.
*   `03_basic_syntax.md`: Variables, annotations, primitive types, pointer types, boolean logic, and control loops.
*   `04_functions.md`: Signatures, return annotations (`->`), named/positional arguments, default values, and FFI annotations.
*   `05_oop_basics.md`: Structs (stack, value) vs. Classes (heap, reference), constructors (`initialize`), destructors (`finalize`), and automated field drops (`__drop_fields`).
*   `06_modules_and_mixins.md`: Modules (static namespaces), Mixins (traits), dynamic dispatch (VTable vs. ITable), and member visibility (private).
*   `07_macros.md`: Macros (`macro`), AST evaluations (`{{expr}}`), control blocks (`{% for %}`, `{% if %}`), and compile-time code generators.
*   `08_frontend_jsx.md`: UI components, reactive state (`state = { ... }`), event listeners, and asynchronous rendering.
*   `09_circuits.md`: Manifest file (`Project.vl`), file-level linking (`@[Link]`), topological dependency sorting, and `volt circuit`.
*   `10_stdlib_shell.md`: System::Shell API, OOP path joining, directory files filtering, subprocess spawning, and pipeline streams.
*   `11_internals.md`: VM bytecode instructions, opcode specifications, register windowing, escape analysis, and scope unwinding.

---

## Verification Workflow

Before final validation of documentation changes:
1.  Verify syntax against [Samples](file:///home/Yutsuna/Projects/Volt/Samples) and tests under [Spec](file:///home/Yutsuna/Projects/Volt/Spec).
2.  Run the test suite using `krystal -s` to ensure compiler features are fully operational.
3.  Update the project's knowledge graph by running `graphify update .`.
