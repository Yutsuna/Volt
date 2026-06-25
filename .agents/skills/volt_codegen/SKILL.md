---
name: volt-codegen
description: Generates or converts logic into correct Volt programming language syntax, enforcing correct OOP paradigms, typing, and RAII structure.
---

# Volt Code Gen Guidelines

Activate this skill when generating samples of Volt language code or checking code syntax for compilers/interpreters.

## Syntax Checklist
*   Ensure no `let`/`var`/`const` keyword is emitted.
*   Functions must use `def` and end with `end`.
*   Verify return type declarations follow the `-> Type` structure.
*   Single-line closures use curly braces: `{ |param| block_body }`.
*   Multi-line closures use `do` / `end` blocks:
    ```volt
    items.each do |item|
      # body
    end
    ```
*   Ensure boolean logic is expressed using `and`, `or`, and `not`.
