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
*   Parameter types use `:` syntax: `def func(param : Int) -> Int`
*   Single-line closures use curly braces: `{ |param| block_body }`.
*   Multi-line closures use `do` / `end` blocks:
    ```volt
    items.each do |item|
      # body
    end
    ```
*   Ensure boolean logic is expressed using `and`, `or`, and `not`.
*   Use `nil` for null values, not `null` or `NULL`.

## Currently Supported Features (v0.1.0)
When generating code for the current interpreter, limit to:
*   Primitive types: `Int64`, `Float64`, `Bool`, `String`, `Nil`
*   Variables: `name = value` or `name : Type = value`
*   Functions: `def name(param : Type) -> ReturnType ... end`
*   Control flow: `if`/`elsif`/`else`, `while`, `until`, `return`
*   Arithmetic: `+`, `-`, `*`, `/`, `%`
*   Comparison: `<`, `<=`, `>`, `>=`, `==`, `!=`
*   Logical: `and`, `or`, `not`
*   Native calls: `@[External("lib")] def func(...) -> Type ... end`

## RAII Considerations
*   Objects will have deterministic lifetimes based on scope
*   INIT/DROP opcodes will be automatically emitted at scope boundaries
*   No explicit memory management required in Volt code
*   Escape analysis will determine stack vs heap allocation

## Code Examples

### Basic Function
```volt
def add(a : Int64, b : Int64) -> Int64
  a + b
end
```

### Conditional Logic
```volt
def max(a : Int64, b : Int64) -> Int64
  if a > b
    a
  else
    b
  end
end
```

### While Loop
```volt
def factorial(n : Int64) -> Int64
  result = 1
  while n > 0
    result = result * n
    n = n - 1
  end
  result
end
```

### External Function
```volt
@[External("libc")]
def puts(str : String) -> Void
end
```
