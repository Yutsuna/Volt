# Functions

Functions in Volt are declared using the `def` keyword, closed with `end`, and are first-class constructs.

---

## Function Definition

A basic function definition specifies parameters with their types using `:` and the return type using `->`.

```volt
def add(a : Int64, b : Int64) -> Int64
  return a + b
end
```

### Implicit Returns
In Volt, the last expression evaluated in a function body is implicitly returned if no explicit `return` statement is encountered. The `return` keyword is optional for simple expressions.

```volt
def multiply(a : Int64, b : Int64) -> Int64
  a * b  # Implicitly returned
end
```

### Void Functions
Functions that perform side effects without returning a value should specify `Void` or `Nil` as their return type. If omitted, the return type defaults to `Nil`.

```volt
def log_message(msg : String) -> Void
  Console.write_line(msg)
end
```

---

## Arguments and Parameters

Volt supports flexible argument invocation styles.

### Parameter Default Values
Parameters can have default values, making them optional during function invocation:
```volt
def greet(name : String, prefix : String = "Hello") -> String
  prefix + ", " + name
end

greet("Léo")                  # Returns "Hello, Léo"
greet("Léo", "Good morning")  # Returns "Good morning, Léo"
```

### Positional vs. Named Arguments
Functions can be invoked using standard positional parameters or explicit named arguments for clarity.
```volt
def create_user(username : String, age : Int64, admin : Bool) -> Void
  # body
end

# Positional invocation:
create_user("admin", 30, true)

# Named argument invocation:
create_user(username: "admin", age: 30, admin: true)
create_user(age: 30, admin: true, username: "admin")  # Order independent
```

---

## Foreign Function Interface (FFI)

Volt programs can invoke native compiled functions directly from shared system libraries (like `libc`) using FFI bindings. This is defined by placing the `@[External]` annotation immediately before a function stub.

### Declaring External Functions
You must declare the function signature without a body.

```volt
# Bind to puts in libc to output strings directly
@[External("libc")]
def puts(str : String) -> Int32

# Bind to malloc in libc for direct memory allocation
@[External("libc")]
def malloc(size : UInt64) -> Void*
```

When the compiler encounters an `@[External("libname")]` annotation, it registers the function in the VM's native lookup table. When the bytecode compiler emits a `CALL_NATIVE` opcode, the VM resolves the symbol dynamically using the system dynamic loader (dlopen/dlsym) and executes the raw compiled function.
