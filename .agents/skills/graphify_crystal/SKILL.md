---
name: graphify-crystal
description: "Instructions for running Graphify with Crystal (.cr) and C (.c/.h/.inc) tree-sitter AST parsing support"
---

# Graphify with Crystal & C Support

This skill provides a single unified runner script (`run_graphify.py`) that extends `graphify` with Crystal (`.cr`) and C (`.c`, `.h`, `.inc`) tree-sitter AST parsing.

## How it works

The default `graphify` package does not natively map `.cr` files to a parser. The runner script handles everything in one pass:

1. **Dependency resolution** : Dynamically discovers `tree_sitter_crystal` Python bindings from local development paths (`~/tree-sitter-crystal/bindings/python`) and Nix store patterns. No hardcoded paths.
2. **Extension registration** : Registers `.cr` and `.inc` in `graphify.detect.CODE_EXTENSIONS`.
3. **Crystal LanguageConfig** : Maps `class_def`, `module_def`, `struct_def`, `alias_def`, and `method_def` tree-sitter node types.
4. **Patched extraction** : Overrides `graphify.extract.extract` with a dispatch table that includes `.cr -> extract_crystal`.
5. **Full pipeline** : Runs AST extraction, graph building, clustering, analysis, and report generation in a single invocation.

## Usage

All commands are run from the project root.

### Default scan (recommended)

Scans `Source/Volt` (`.cr`) and `Source/C` (`.c`, `.h`, `.inc`), excludes `Benchmarks/` and `Std/`:

```bash
python3 .agents/skills/graphify_crystal/run_graphify.py
```

### Custom targets

Specify which directories to scan:

```bash
python3 .agents/skills/graphify_crystal/run_graphify.py Source/Volt Source/C
```

### Custom exclusions

Exclude specific directories from the scan:

```bash
python3 .agents/skills/graphify_crystal/run_graphify.py --exclude Benchmarks Core Tests
```

### Custom output directory

Change where outputs are saved (default: `graphify-out/`):

```bash
python3 .agents/skills/graphify_crystal/run_graphify.py --output-dir my-graph-output
```

### Full example with all options

```bash
python3 .agents/skills/graphify_crystal/run_graphify.py \
  Source/Volt Source/C \
  --exclude Benchmarks Core \
  --output-dir graphify-out
```

## Outputs

All outputs are saved in the `--output-dir` directory (default `graphify-out/`):

| File               | Description                                        |
|--------------------|----------------------------------------------------|
| `graph.json`       | Full knowledge graph (nodes, edges, communities)   |
| `graph.html`       | Interactive visualization (skipped if > 5000 nodes)|
| `GRAPH_REPORT.md`  | Markdown report with god nodes, communities, etc.  |
| `cache/`           | Per-file extraction cache (hash-based)             |

## Prerequisites

- `graphify` must be available in the Python environment (provided by the Nix shell).
- `tree_sitter_crystal` Python bindings must be discoverable. The script checks:
  - `~/tree-sitter-crystal/bindings/python`
  - `./tree-sitter-crystal/bindings/python`
  - Nix store glob patterns for `python-tree-sitter-crystal` packages
