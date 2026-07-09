# Basic Syntax

Volt is designed to be highly readable, favoring clean keywords and minimal syntactic clutter.

---

## Variables

Variables in Volt do not require explicit declaration keywords like `let`, `var`, `val`, or `const`. Assignment defines the variable name and infers its type.

### Implicit Type Inference
```volt
message = "Hello World"  # Inferred as String
age = 42                 # Inferred as Int64
ratio = 3.14             # Inferred as Float64
active = true            # Inferred as Bool
```

### Explicit Type Annotation
To enforce strict contracts or document code, types can be annotated using the `:` syntax:
```volt
count : Int64 = 100
price : Float64 = 19.99
label : String = "Title"
```

---

## Primitive Types

Volt supports a concise set of built-in primitive types:

| Type | Description | Example |
| :--- | :--- | :--- |
| `Int64` | Signed 64-bit integer | `42`, `-100` |
| `Float64` | Double-precision 64-bit floating point | `3.14`, `-0.001` |
| `Bool` | Boolean logic | `true`, `false` |
| `String` | UTF-8 encoded text sequence | `"Volt Language"` |
| `Nil` | The absence of a value | `nil` |

### Pointer Types
Volt provides low-level raw pointers for performance-critical scenarios and FFI bindings. Pointers are annotated using the type followed by an asterisk `*`:
*   `Int64*`: A raw pointer to an integer.
*   `Void*`: An untyped raw pointer.

Pointers are instantiated using the address-of operator `&` and dereferenced with the prefix dereference operator `*`:
```volt
x = 42_i64
ptr : Int64* = &x  # Store address of x
val = *ptr         # Dereference ptr (val becomes 42)
```

---

## Operators

Volt provides a complete set of arithmetic, comparison, and logical operators.

### Arithmetic Operators
Arithmetic is strongly typed. Mixing incompatible types (like adding `Int64` and `Float64`) without explicit casting triggers a compile-time type mismatch.
*   `+`: Addition
*   `-`: Subtraction
*   `*`: Multiplication
*   `/`: Division
*   `%`: Modulo

### Comparison Operators
All comparison operators evaluate to a `Bool` value:
*   `<`: Less than
*   `<=`: Less than or equal to
*   `>`: Greater than
*   `>=`: Greater than or equal to
*   `==`: Equal to
*   `!=`: Not equal to

### Logical Operators
Volt supports English logical operators for improved readability. Traditional symbols are also parsed, but English keywords are preferred in idiomatic Volt:
*   `and` (alias of `&&`): Logical conjunction.
*   `or` (alias of `||`): Logical disjunction.
*   `not` (alias of `!`): Logical negation.

```volt
if logged_in and not banned
  allow_access()
end
```

---

## Control Flow

Control flow structures in Volt do not use parentheses around conditions, and their bodies are closed with the `end` keyword.

### Conditional Branching (`if`, `elsif`, `else`)
```volt
if score > 90
  grade = "A"
elsif score > 80
  grade = "B"
else
  grade = "C"
end
```

### Loops (`while`, `until`)
Volt supports both positive and negative conditional loops.

*   `while`: Continues execution as long as the condition evaluates to `true`.
    ```volt
    i = 0
    while i < 10
      i = i + 1
    end
    ```
*   `until`: Continues execution as long as the condition evaluates to `false` (equivalent to `while not condition`).
    ```volt
    finished = false
    until finished
      if check_status()
        finished = true
      end
    end
    ```
