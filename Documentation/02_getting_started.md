# Getting Started

This guide details how to configure the Volt toolchain, compile programs, run test suites, and interact with the interactive REPL.

---

## Toolchain Prerequisites

Volt is implemented in Crystal and compiled using `krystal`. To build, compile, or test Volt, you must use the `krystal` wrapper commands.

### Compilation and Testing Commands

*   **Run Spec Tests**: Execute the test suite using:
    ```bash
    krystal -s
    ```
*   **Compile and Execute Entrypoint**: Compile the compiler and run the binary with default parameters:
    ```bash
    krystal -x
    ```
*   **Compile Release Binary**: Build an optimized production binary under `bin/Volt`:
    ```bash
    krystal -r
    ```
*   **List Options**: List the options for the compiler:
    ```bash
    krystal --help
    ```

---

## Command Line Interface (CLI)

Once compiled, the Volt compiler generates a CLI executable at `bin/Volt`. The CLI supports the following subcommands:

### `volt run`
Interprets and executes a Volt program.
```bash
./bin/Volt run source_file.vl
```
If a project contains a `Project.vl` manifest file, executing `./bin/Volt run` without arguments will search upward for the manifest, load the logical entrypoint, and run the multi-file project.

### `volt parse`
Parses a source file and dumps its Abstract Syntax Tree (AST) to the stdout in a readable format. Excellent for debugging grammar and parser issues.
```bash
./bin/Volt parse source_file.vl
```

### `volt check`
Runs the semantic analysis and type-checking phases without executing the bytecode. This validates variables, types, vtable structures, and checks for compile-time errors.
```bash
./bin/Volt check source_file.vl
```

### `volt repl`
Launches the interactive Read-Eval-Print Loop (REPL) shell.
```bash
./bin/Volt repl
```

### `volt circuit`
Scans the directory tree under `Source/`, identifies source modules, and updates or generates the project manifest file (`Project.vl`).
```bash
./bin/Volt circuit
```

---

## The Interactive REPL

The Volt REPL provides an incremental compilation environment. Unlike traditional scripting REPLs that evaluate code line-by-line via slow interpretation, the Volt REPL compiles inputs into bytecode on-the-fly.

### Incremental State
The REPL uses an `IncrementalAnalyser` and `REPLDeltaCompiler`. Global variables and function definitions are preserved and can be referenced across inputs. 

### Redefining Functions
You can redefine functions in subsequent lines of the REPL. The runtime signature table updates the dispatch target to point to the new bytecode chunk, while existing instances or stack frames are unaffected.
```volt
def greet -> String
  "Hello"
end

greet() # => "Hello"

# Redefining the function:
def greet -> String
  "Hi!"
end

greet() # => "Hi!"
```
*Note: Redefining classes, structs, mixins, or modules is intentionally rejected with compiler error `S0003` to prevent memory corruption and use-after-free hazards on active instances.*

### Special REPL Commands

All REPL control commands are prefixed with a colon `:`:

*   `:load <file>`: Loads and compiles a Volt source file into the current REPL session.
*   `:reload`: Reloads all previously loaded files, resolving modifications dynamically.
*   `:clear`: Resets the REPL session state, clearing all compiled types, globals, and signature mappings.
*   `:exit`: Quits the REPL session.
*   `:help`: Displays available commands and usage hints.
