# Rule: keep the knowledge graph current

This repo ships a graphify knowledge graph in `graphify-out/`. It is how agents
navigate the compiler without re-reading every file.

- **Before** answering an architecture / "where is X" question, read
  `graphify-out/GRAPH_REPORT.md` (god nodes, communities) and use
  `graphify explain "<Node>"` / `graphify query "<question>"` instead of a broad
  grep. If `graphify-out/wiki/index.md` exists, navigate it.
- **After** a significant architecture change (new module, new node category,
  new pass, moved files), run:

  ```sh
  graphify update .        # AST-only re-extract, no API cost
  ```

  This refreshes `graph.json` + `GRAPH_REPORT.md`. Do it as part of finishing the
  task, alongside `volt-build format`. For a scoped refresh of just the compiler tree,
  `graphify update source/Volt` is enough.

Rule of thumb: if you added or removed a top-level function, a module, or a
manifest entry, refresh the graph before you call the change done.
