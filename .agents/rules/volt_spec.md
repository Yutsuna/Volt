# Volt — Language Design Assistant

## Context

You are helping design and implement **Volt**, a new high-level scripted/compiled programming language. Your role is to maintain consistency across all language decisions, propose syntax, write spec sections, and help prototype the compiler/interpreter.

**Current Version:** v0.1.0 (Tier-0 MVP)
**Status:** End-to-end execution path is live and working

---

## Build & Test Commands

To compile, run, or test the project, you **must** use `krystal` instead of `crystal`.

- Compile and run: `krystal -x`
- Compile release mode: `krystal -r`
- Compile and run tests (specs): `krystal -s`
- See all options: `krystal --help`

**CLI Commands (after building):**
- `./bin/Volt run <file.volt>` — Interpret a Volt program
- `./bin/Volt ast <file.volt>` — Dump abstract syntax tree
- `./bin/Volt analyse <file.volt>` — Run semantic analysis
- `./bin/Volt repl` — Start interactive REPL
- `./bin/Volt version` — Show version
- `./bin/Volt help` — Show all commands

---


## What Volt is

Volt is a high-level, fully object-oriented language with the following properties:

- **Scripted by default**, with optional native compilation (via LLVM) and WASM target
- **Syntax inspired by Ruby and Crystal** — expressive, readable, English-like
- **Gradually typed** — type inference by default, explicit annotations optional
- **Frontend-capable** — JSX-like syntax for UI components, targets Web via WASM
- **Shell-first stdlib** — a fully typed OOP API for filesystem and process manipulation

---

## Core syntax rules

### Variables — no keyword required
```volt
message = "Hello World"   # inferred String
age : Int = 28            # explicit type annotation
```

### Functions — `def`, return type with `->`
```volt
def add(a : Int, b : Int) -> Int
  a + b
end

def greet(name : String) -> Void
  Console.write_line("Hello, #{name}")
end
```

### Blocks, lambdas, and chaining — Ruby/Crystal style
```volt
numbers.map { |it| it * 2 }.select { |it| it > 5 }.to_s

double = { |x| x * 2 }
```

### Boolean operators — aliased, English-first
| Symbol | Alias |
|--------|-------|
| `&&`   | `and` |
| `\|\|`  | `or`  |
| `!`    | `not` |

```volt
if logged_in and not banned
  allow_access
end

name = user?.profile?.name or "Guest"
```

### Classes, mixins, generics

```volt
mixin Printable
  def to_s -> String
    "(Printable)"
  end
end

class Box[T] include Printable
  value : T

  def initialize(@value : T)
  end

  def map[U](block : T -> U) -> Box[U]
    Box.new(block(@value))
  end
end
```

```volt
abstract class Animal
  abstract def speak() -> String
end

class Dog < Animal
  def speak() -> String
    "Woof!"
  end
end
```

### Constructors and destructors

Initialize is called at the object creation.
Finalize is called at the object destruction (RAII)

```volt
class User

  # STYLE 1: all parameters are instance variables, no need to repeat them
  def initialize(@name : String, @email : String, @admin : Bool = false)
  end
  # END STYLE 1

  # STYLE 2: parameters are not instance variables, must assign them
  name : String
  email : String
  admin : Bool

  def initialize(name : String, email : String, admin : Bool = false)
    @name = name
    @email = email
    @admin = admin
  end
  # END STYLE 2

  def finalize
    # Called at destruction
  end
end

user1 = User.new("Léo", "leo@beaugosse.com", false)
user2 = User.new(name: "Léo", email: "leo@beaugosse.com", admin: false)
```

Volt doesnt have a garbage collector.
Volt uses RAII (Resource Acquisition Is Initialization) and deterministic destruction.
When an object goes out of scope, its `finalize` method is called automatically.
Like in C++.

### Pattern matching
```volt
match response.status
  when 200 then response.body
  when 404 then raise NotFound
  when 500..599 then raise ServerError
  else raise UnknownError
end
```

### Pipe operator
```volt
result = raw_text
  |> strip
  |> downcase
  |> split(",")
  |> map { |s| s.trim }
```

### Async / await
```volt
async def fetch_user(id : Int) -> User
  data = await http.get("/users/#{id}")
  User.from_json(data)
end
```

### FFI — External bindings
```volt
@[External("libc")]
def puts(str : String) -> Void

@[External]
def strlen(s : String) -> Int
```

### Compile-time annotations
```volt
@[Inline]
def fast_sum(a : Int, b : Int) -> Int
  a + b
end

@[Export("volt_add")]
def add(a : Int32, b : Int32) -> Int32
  a + b
end
```

### CLI
```sh
volt run script.volt          # scripted mode
volt build main.volt -o app   # native binary
volt build --target wasm      # WebAssembly
volt circuit                  # auto-resolve modules and sync with Project.vl
```

---

## Implementation Status (v0.1.0)

### Fully Working
All syntax examples in the following sections are implemented and tested:
- Variables — no keyword required
- Functions — `def`, return type with `->`
- Blocks, lambdas, and chaining — Ruby/Crystal style
- Boolean operators — aliased, English-first
- Arithmetic operators: `+`, `-`, `*`, `/`, `%`
- Comparison operators: `<`, `<=`, `>`, `>=`, `==`, `!=`
- Unary operators: `-`, `!`
- Control flow: `if`/`elsif`/`else`
- Loops: `while`
- Return statements
- Native calls via `@[External]`

### Partially Working
- Control flow: `until` — implemented but less tested
- Logical operators: `and`, `or`, `not` — working but `not` uses `!` internally

### Not Yet Implemented
- Classes, mixins, generics
- Pattern matching (`match`/`when`)
- Async / await
- FFI — External bindings (only basic `@[External]` works)
- Compile-time annotations (`@[Inline]`, `@[Export]`)
- Pipe operator (`|>`)
- Frontend — UI components
- Project Configuration (`Project.vl`) & `volt circuit`
- Shell OOP API — `System/Shell`

The `Project.vl` is a compilable Volt file acting as the project manifest, defining architecture, modules, and external dependencies.

### Example `Project.vl`
```volt
circuit "MyApp"
{
    runtime "0.1.0"
    modules(
        "Core"    => "Source/Core"
        "Models"  => "Source/Models"
        "Network" => "Source/Lib/Network"
    )
    battery "json", version: "1.0.0"
}
```

### `volt circuit` Command
The `volt circuit` command automates project maintenance and module mapping:
- **Auto-resolution:** Scans the file tree under `Source/`, detects new directories, and updates the `modules(...)` mapping in `Project.vl`.
- **Preservation:** Intelligently preserves custom configurations and dependencies (`battery` declarations).
- **Usage:** Run `volt circuit` to sync logical namespace mappings without manual editing.

---

## Frontend — UI components

Components are declared with `component`, not `class` or `def`, so the compiler can treat them distinctly from pure functions.

```vlx
component UserCard(user : User)
  label = user.admin? ? "Admin" : "Member"

  return <div class="card">
    <h2>{ user.name }</h2>
    <span class="badge">{ label }</span>
    <button on:click={ logout }>Sign out</button>
  </div>
end

# Fragment shorthand
component Layout()
  return <>
    <Header title="Home" />
    <Main />
  </>
end
```

Async components:
```vlx
async component UserCard(id : Int)
  user = await fetch_user(id)
  return <div>{ user.name }</div>
end
```

---

## Shell OOP API — `System::Shell`

The shell stdlib exposes filesystem, process, and IO as fully typed, chainable objects.

```volt
# Directory traversal
Directory.current
  .files(recursive: true)
  .filter { |file| file.extension == "log" and file.size > 10.megabytes }
  .each { |file|
    destination = Directory.home / "archives" / file.name
    file.move_to(destination)
    Console.write_line("Archived: #{file.name} (#{file.size.to_human_string})")
  }

# Glob
Path.glob("**/*.tmp").each(&:delete)

# Process
pid = Proc.spawn("git status")
pid.wait.stdout.lines.first

# Shell pipe
out = Shell.pipe(
  "find . -name '*.volt'",
  "xargs wc -l"
)
```

**Design goals vs PowerShell:**
- Return types are guaranteed at compile time — no implicit string coercion
- Every object in the pipeline is strongly typed and IDE-completable
- Chainable API reduces token count for AI-driven code traversal

---

## What to keep consistent

When proposing new syntax or APIs, always:

1. Prefer English readability over symbol density
2. Use `def` for all functions — never `fn`, `func`, or `function`
3. Use `->` for return types, `:` for parameter types
4. Use `end` to close all blocks — never `}`  except inline lambdas `{ |x| ... }`
5. Prefer method chaining over nested calls
6. Keep annotations in `@[AnnotationName]` form
7. No `let`, `var`, or `val` — plain assignment always
8. `and`, `or`, `not` preferred over `&&`, `||`, `!` in prose code

---

## Crystal Coding Standards

For the Crystal codebase of this project, we follow these specific style and coding rules:

### Spacing & Formatting
* **Module Declarations**: Leave exactly 2 blank lines after a module declaration.
* **Class Declarations**: Leave exactly 2 blank lines between each class.
* **Parentheses & Brackets**: Put spaces inside parentheses and brackets for function definitions, parameters, and array/hash access.
  ```crystal
  def my_function( param : String, param2 : Int ) : ReturnValue
    param[ param2 ] = param2
  end
  ```

### Naming Conventions
* **Abstract Classes**: All abstract class names must start with the capital letter `A`.
  ```crystal
  abstract class AHuman
    property name
  end

  class Alice < AHuman
  end
  ```

### JSX

```vlx
# IDEA: maybe translate the component JSX to:
#
# class Counter < Volt::JSX::Component
#   property count        : Int32
#   property on_increment : -> Void
#
#   def initialize( @count, @on_increment ); end
#
#   def render : Volt::JSX::HtmlNode
#     Volt::JSX.element("div", { "class" => "counter" }, [
#       Volt::JSX.element("button", { "on:click" => @on_increment }, [
#         Volt::JSX.text("+1")
#       ]),
#       Volt::JSX.element("span", {}, [
#         Volt::JSX.text("Count: "),
#         Volt::JSX.text(@count.to_s)
#       ])
#    ])
#   end

component Counter( count : Int, on_increment : -> Void )

  <div class="counter">
    <button on:click={on_increment}>+1</button>
    <span>Count: {count}</span>
  </div>

end
```
