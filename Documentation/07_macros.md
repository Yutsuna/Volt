# Macros and Compile-Time Metaprogramming

Volt features a macro system inspired by Crystal, enabling powerful compile-time code generation, AST manipulation, and conditional compilation.

---

## 1. Macro Basics

A macro is defined using the `macro` keyword and closed with `end`. Unlike runtime functions, macros generate code that is injected directly into the AST during compilation.

### Syntax
```volt
macro log_info(expression)
  puts("[INFO] " + __FILE__ + " (line " + __LINE__.to_s + ") : " + {{expression}})
end
```

### Macro Variables
Volt provides built-in compile-time constants:
*   `__FILE__`: Evaluates to a string literal containing the path of the current source file.
*   `__LINE__`: Evaluates to an integer literal representing the current line number in the source file.

---

## 2. Macro Expansion and Interpolation

We control macro expansion using two kinds of delimiters:

### Evaluation Delimiter `{{ ... }}`
Expressions enclosed in double curly braces `{{ ... }}` are evaluated at compile time, and their resulting AST representation is inserted at the evaluation site.

#### Identifier Expansion (`.id`)
If we interpolate a string directly into code, the compiler retains the quotes (e.g. producing `"my_method"`). To parse the content as a raw identifier (e.g. producing `my_method`), append the `.id` method:
```volt
macro define_method(name)
  def {{name.id}}() -> Void
    puts("Method called!")
  end
end

define_method("test")
# Generates:
# def test() -> Void
#   puts("Method called!")
# end
```

#### Compile-Time String Methods
Basic string methods like `.upcase` or `.downcase` can be called on macro arguments at compile time:
```volt
macro announce(message)
  puts({{message.upcase}})
end

announce("hello") # Expands to: puts("HELLO")
```

---

## 3. Compile-Time Control Flow

Volt allows conditional code generation and loop unrolling inside macros using the `{% ... %}` control delimiters.

### Conditional Statements (`{% if %}`)
Branching selects which code block to emit based on compile-time expressions:
```volt
macro describe(kind)
  {% if kind == "fruit" %}
    puts("It is a fruit.")
  {% elsif kind == "vegetable" %}
    puts("It is a vegetable.")
  {% else %}
    puts("Unknown category.")
  {% end %}
end

describe("fruit")      # Expands to: puts("It is a fruit.")
describe("mineral")    # Expands to: puts("Unknown category.")
```

### Loop Unrolling (`{% for %}`)
Loops iterate over lists of compile-time values, replicating the body for each element:
```volt
macro generate_levels()
  {% for level in ["low", "medium", "high"] %}
    puts("Level: " + {{level.upcase}})
  {% end %}
end

generate_levels()
# Expands to:
# puts("Level: " + "LOW")
# puts("Level: " + "MEDIUM")
# puts("Level: " + "HIGH")
```

---

## 4. Code Generation Patterns

Macros can dynamically compose method names, variables, and classes.

```volt
macro define_greeters(names)
  {% for name in names %}
    def greet_{{name.id}}() -> Void
      puts("Greetings, " + {{name}} + "!")
    end
  {% end %}
end

define_greeters(["world", "volt", "developer"])

# This invocation generates three complete function definitions:
# greet_world()
# greet_volt()
# greet_developer()
```

By performing evaluation during compile-time, macros introduce zero runtime performance overhead, acting as clean, compiler-guided code generators.
