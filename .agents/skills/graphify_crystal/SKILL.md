---
name: graphify-crystal
description: "Instructions for running Graphify with Crystal (.cr) tree-sitter AST parsing support"
---

# Graphify with Crystal Support

This skill enables running `graphify` on Crystal codebases (files ending in `.cr`) by registering the `tree_sitter_crystal` grammar module and mapping it into the generic extractor.

## How it works

Since the default Python `graphify` package doesn't natively map `.cr` files to a parser, a custom runner script `run_graphify.py` is stored in this directory to do the following:

1. Dynamically append Nix store python package paths (including `python-tree-sitter-crystal`) to the site directory path list.
2. Register `.cr` in `graphify.detect.CODE_EXTENSIONS`.
3. Define the `LanguageConfig` for Crystal mapping `class_def`, `module_def`, `struct_def`, and `method_def` nodes.
4. Set up `graphify.extract._DISPATCH['.cr'] = extract_crystal`.
5. Call the `graphify` entry point.

## Usage

To rebuild the graph for the current repository with Crystal AST parsing enabled, run:

```bash
# 1. Build/Extract the graph
/nix/store/l9k0anq0z7zz81zcwy035jfwap9ga6rl-python3-3.13.13/bin/python3 .agents/skills/graphify_crystal/run_graphify.py .

# 2. Cluster the graph and generate report outputs
/nix/store/l9k0anq0z7zz81zcwy035jfwap9ga6rl-python3-3.13.13/bin/python3 .agents/skills/graphify_crystal/run_graphify.py cluster-only .
```

Outputs will be saved in `graphify-out/` as usual.
