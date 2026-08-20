title:	Fix(Macros): Compile-time macros with REAL RESOLUTIONS
state:	OPEN
author:	Yutsuna (Yutsuna)
labels:	enhancement
comments:	0
assignees:	Yutsuna (Yutsuna)
projects:	
milestone:	
issue-type:	
parent:	
sub-issues:	
sub-issues-completed:	
blocked-by:	
blocking:	
number:	75
--
### Overview

This proposal outlines the design for Volt's macro system and compile-time evaluation model. The objective is to provide powerful metaprogramming and compile-time execution capabilities while keeping the language grammar clean, explicit, and minimal.

To avoid keyword inflation (such as `comptime`, `constexpr`, or `#define`) and template tag verbosity (such as `{% %}` or `{{ }}`), the entire macro system is built around a single unified keyword: `macro`.

---

### Design Principles

1. **Single Keyword Focus:** Metaprogramming constructs rely exclusively on the `macro` keyword.
2. **Direct Method Generation:** `macro def` directly defines a compile-time evaluated method signature, eliminating the indirection of wrapper macro functions that emit nested `def` blocks.
3. **Integrated Shell Execution:** Native support for backtick syntax (`` `command` ``) executed at compile time by the compiler's execution engine.
4. **Natural Control Flow:** Standard Volt control structures (`if`, `for`, `case`) evaluated within a `macro` context serve as compile-time branching and iteration, eliminating the need for dedicated directive keywords (e.g., `macro if`).

---

### Language Specification

#### 1. Compile-Time Method Definitions (`macro def`)

A `macro def` construct declares a method whose body is executed at compile time. The resulting AST or returned expression is emitted directly as the implementation for the target type.

When used inside mixins or classes, the macro inspects the target type at compile time and expands into concrete runtime code.

```vl
mixin Serializable
  macro def to_json -> String
    json = "{"
    for field in self.fields
      json += "\"#{field.name}\":" + @#{field.name}.to_string + ","
    end
    json.chomp(",") + "}"
  end

  macro def from_json( json : String ) -> self
    # Compile-time deserialization AST generation
  end
end

class User
  include Serializable

  getter name : String
  getter age  : Int32

  def initialize( @name : String, @age : Int32 )
  end
end
```

#### 2. Compile-Time Shell Execution (Backticks)

Backticks executed within a macro context run shell commands on the host machine during compilation. The output is available to the compiler as standard Volt string data or arrays of strings via `.lines`.

```vl
macro def register_test_suite -> Void
  # Executed on host during compilation
  test_files = `find #{__DIR__}/tests -type f -name "*.vl"`.lines

  for file in test_files
    test_name = file.basename(".vl")
    puts "Generating test fixture for #{test_name}..."
    
    # Read file content at compile-time and embed assertion
    content = `cat #{file}`
    assert!( content.size > 0 )
  end
end
```

Global constants can also evaluate shell commands at compile time:

```vl
BUILD_COMMIT : String = `git rev-parse --short HEAD`.trim
BUILD_DATE   : String = `date -u +"%Y-%m-%dT%H:%M:%SZ"`.trim
```

#### 3. Conditional Compilation via Standard Control Flow

Separate conditional macros (e.g., `macro if`) are omitted in favor of standard Volt `if` statements executed inside a `macro def` or `macro` block. Only the branch resolved during compile-time evaluation is emitted into the compiled binary.

```vl
macro def system_exit( code : Int32 ) -> Void
  if `uname`.trim == "Linux"
    libc_linux_exit( code )
  else
    libc_macos_exit( code )
  end
end
```

#### 4. Top-Level Macro Execution (`macro do`)

For compile-time actions that do not bind to a specific method signature (such as build validation, code generation hooks, or environmental checks), the `macro do ... end` block is used at the file level.

```vl
macro do
  file_count = `find #{__DIR__} -name "*.vl" | wc -l`.trim
  puts "Compiling Volt project across #{file_count} source files..."
end
```

---

### Benefits

- **Zero-Boilerplate Syntax:** Eliminates the need to nest `def` declarations inside macro generators.
- **Consistent Grammar:** Uses standard Volt control structures and string interpolation rather than introduce a separate template syntax.
- **Minimal Surface Area:** Avoids keyword bloat by reusing `macro` for all compile-time contexts.
- **Host Integration:** Enables clean scripting and file generation workflows via compile-time backtick execution.

