# Rule: cli-surface — the `volt` command contract

The compiler front-end is a **multi-command CLI**. Every user-facing feature
lands behind one of these subcommands; do not invent new top-level flags on
`main` outside this contract. `Volt/Private/Main.cpp` is the only place that
parses argv, and it must stay a thin dispatcher onto `Driver`.

```
Volt Language
Usage: volt <command> [options]
  build         Compile the file input
  build-stdlib  Pre-compile the standard library artifact
  check         Static semantic code analysis
  circuit       Create or update the Project.vl file
  format        Run the formatter on the code
  help          Display the usage
  parse         Generate & display the abstract syntax tree
  repl          Interactive Read-Eval-Print-Loop
  run           Compile and run the file input
  version       Prints the current version of Volt
```

All commands will exist eventually. **Current priority order:** `run`, `repl`,
`parse`, `check`, `version`, `help`, `circuit` — then `build`, `format`.

## Per-command options (contract)

```
Usage: volt circuit [options]
    -d DIR, --dir DIR                Project directory path
    -h, --help                       Show help

Usage: volt run [options] [input_file] [-- ...]
    -i INPUT, --input INPUT          File input source program
    -s, --stdin                      Read input from stdin
    -w, --watch                      Recompile and hot-reload on file change; the
                                     program runs on its own thread, so one that
                                     never returns is reloaded in place
    --no-lazy                        Compile every function up front, not on its first call
    -h, --help                       Show help

Usage: volt repl [options] [file]
    -i INPUT, --input INPUT          Pre-load a file into the REPL session
    -n, --no-history                 Disable history saving
    -h, --help                       Show help

Usage: volt parse [options] [input_file]
    -i INPUT, --input INPUT          Source input module path
    -o OUTPUT, --output OUTPUT       Output target path structure
    --format FORMAT                  Serialization formats (json|dot|text)
    --simplify                       Deduplicate structural tree layout elements
    --lowered                        Display the tree after the AST lowering passes
    --resolved                       Display the tree after full resolution and inlining (Core AST)
    --no-color                       Output without colors
    --no-location                    Omit character and index coordinates
    -h, --help                       Show help

Usage: volt check [options] [input_file_or_dir]
    -i INPUT, --input INPUT          Code target directory or source file
    --type TYPE                      Type verification scope (syntax|semantic|style)
    --rules RULES                    Location path defining validation rule models
    --output OUT                     Output validation layout formats
    --warn-as-error                  Style warning events will promote to runtime errors
    --metrics                        Gather code statistics and size metric benchmarks
    --unused                         Flag unreferenced syntax structures and bindings
    -h, --help                       Show help

Usage: volt build [options] [input_file]
    -i INPUT, --input INPUT          File input source program
    -o OUTPUT, --output OUTPUT       Output artifact path
    --target TARGET                  Code generation target (native|wasm)
    -O LEVEL                         Optimization level (0|1|2|3, default 2)
    --emit KIND                      Stop after an intermediate artifact (ast|ir|obj)
    --lto                            Enable link-time optimization (native only)
    -h, --help                       Show help

Usage: volt build-stdlib [options]
    --kind KIND                      Artifact kind (static|shared, default shared)
    -O LEVEL                         Optimization level (0|1|2|3, default 2)
    --fresh                          Discard and rebuild the cached artifact
    -h, --help                       Show help
```

## How this maps onto the architecture

- `parse`  → Driver parse only: the raw AST is the product (tooling needs the
  structured JSX nodes). `--lowered` additionally runs the `EPassKind::Lowering`
  passes from `PassList.inl` — never full sema. Serialisation via the dumper
  (`text` today; `json`/`dot` are printer back-ends, not new traversals).
- `check`  → Driver parse + MiddleEnd passes; `--type` selects which pass orders run.
- `run` / `repl` → `Driver::Run` / `Driver::OpenReplSession`, on top of the same
  Driver pipeline (one `CompileUnit` per input, REPL = incremental units), then
  `BackendJIT` through the `IJitBackend` seam (`backend/jit.md`). Neither takes
  `--target`: the backend for these two is not user-selectable. `run --watch`
  adds hot reload, and runs the program on a thread of its own so a program that
  never returns can still be reloaded into; both are unavailable in a build
  configured without LLVM or without the JIT, and say so rather than failing
  obscurely.
- `circuit` → `Driver::CompileCircuit` / Project.vl scaffolding.
- `build` → the full Driver pipeline, then a code generator selected by
  `--target` through the `IBackend` seam (`BACKEND.md`): `native` →
  `BackendLLVM`, `wasm` → `BackendWASM`.
- `build-stdlib` → warms the native stdlib artifact cache that `volt run` reads
  and `volt build` links (`EnsureStdlibArtifact`); it is not a second pipeline.
- `version` → `VERSION.md` is the single source of the version string.

Meta-first applies here too: a new subcommand is **one `IGenericCommand`
subclass plus one `TCommandRegister<T>` static** at the bottom of its `.cpp`
(`CLI/CommandRegistry.hpp`) — never a new ad-hoc argv loop, and never an edit to
`Main.cpp`, which only looks the name up in the registry. Sources are globbed,
so there is no build-file edit either. Shared option groups
(`GetInputOptions`, `StdlibCacheOptions`) are appended rather than retyped.
