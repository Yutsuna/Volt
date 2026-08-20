# Rule: cli-surface — the `volt` command contract

The compiler front-end is a **multi-command CLI**. Every user-facing feature
lands behind one of these subcommands; do not invent new top-level flags on
`main` outside this contract. `Volt/Private/Main.cpp` is the only place that
parses argv, and it must stay a thin dispatcher onto `Driver`.

```
Volt Language
Usage: volt <command> [options]
  build    Compile the file input
  check    Static semantic code analysis
  circuit  Create or update the Project.vl file
  format   Run the formatter on the code
  help     Display the usage
  parse    Generate & display the abstract syntax tree
  repl     Interactive Read-Eval-Print-Loop
  run      Interpret the file input
  version  Prints the current version of Volt
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
```

## How this maps onto the architecture

- `parse`  → Driver parse only: the raw AST is the product (tooling needs the
  structured JSX nodes). `--lowered` additionally runs the `EPassKind::Lowering`
  passes from `PassList.inl` — never full sema. Serialisation via the dumper
  (`text` today; `json`/`dot` are printer back-ends, not new traversals).
- `check`  → Driver parse + MiddleEnd passes; `--type` selects which pass orders run.
- `run` / `repl` → the later JIT/interpreter phase; both sit on top of the same
  Driver pipeline (one `CompileUnit` per input, REPL = incremental units).
- `circuit` → `Driver::CompileCircuit` / Project.vl scaffolding.
- `build` → the full Driver pipeline, then a code generator selected by
  `--target` through the `IBackend` seam (`BACKEND.md`): `native` →
  `BackendLLVM`, `wasm` → `BackendWASM`. `volt run`'s VM is the same seam
  with `BackendVM`.
- `version` → `VERSION.md` is the single source of the version string.

Meta-first applies here too: a new subcommand is **one entry in the command
table of Main.cpp** (name, summary, option spec, entry function) — never a new
ad-hoc argv loop.
