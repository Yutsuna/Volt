---
name: krystal-runner
description: Runs compile, execution, and test commands for the Volt Crystal-based codebase using the custom krystal compiler tool.
---

# Krystal Runner Instruction Set

Use this skill when you need to verify, build, or test the interpreter and VM files using the custom `krystal` compiler.

## Core Commands
*   **Run Specs (Tests):** `krystal -s`
*   **Compile and Execute:** `krystal -x`
*   **Build Release Version:** `krystal -r`
*   **Print Compiler Options:** `krystal --help`

## Execution Guidelines
1. Always verify modifications to the parser, IR, compiler, or VM by running `krystal -s`.
2. Do not use standard `crystal` commands; the project environment relies strictly on `krystal` command-line switches.
