---
name: krystal-runner
description: Runs compile, execution, and test commands for the Volt Crystal-based codebase using the custom krystal compiler tool.
---

# Krystal Runner Instruction Set

Use this skill when you need to verify, build, or test the interpreter and VM files using the custom `krystal` compiler.

## Core Commands
*   **Run Specs (Tests):** `krystal -s` : Runs all Crystal specs in the Spec/ directory
*   **Compile and Execute:** `krystal -x` : Compiles and runs the main Volt entry point
*   **Build Release Version:** `krystal -r` : Builds optimized release binary
*   **Print Compiler Options:** `krystal --help` : Shows all available options

## CLI Commands (via ./bin/Volt)
Once built, you can also test Volt programs directly using the CLI:
*   **Run Volt Program:** `./bin/Volt run <file.volt>` : Interpret a Volt source file
*   **Dump AST:** `./bin/Volt ast <file.volt>` : Show abstract syntax tree
*   **Semantic Analysis:** `./bin/Volt check <file.volt>` : Run semantic analysis only
*   **REPL:** `./bin/Volt repl` : Start interactive Volt REPL
*   **Version:** `./bin/Volt version` : Show Volt version
*   **Help:** `./bin/Volt help` : Show all CLI commands

## Execution Guidelines
1. Always verify modifications to the parser, IR, compiler, or VM by running `krystal -s`.
2. Do not use standard `crystal` commands; the project environment relies strictly on `krystal` command-line switches.
3. For end-to-end testing, use `./bin/Volt run <test_file.volt>` to verify Volt program execution.
4. After significant changes, run both `krystal -s` (unit tests) and manual Volt program tests.
