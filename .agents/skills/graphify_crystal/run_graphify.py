#!/usr/bin/env python3

import sys
import site
import os
import json
import shutil
import re
import glob
import time
import argparse
from pathlib import Path


def setup_paths():
    graphify_bin = shutil.which("graphify")
    if graphify_bin:
        real_graphify = os.path.realpath(graphify_bin)
        wrapped_path = None
        try:
            with open(real_graphify, "r") as f:
                wrapper_content = f.read(4096)

            match_wrapped = re.search(r'exec -a "\$0"\s+"([^"]+\.graphify-wrapped)"', wrapper_content)
            if match_wrapped:
                wrapped_path = match_wrapped.group(1)
            else:
                potential_wrapped = os.path.join(os.path.dirname(real_graphify), ".graphify-wrapped")
                if os.path.exists(potential_wrapped):
                    wrapped_path = potential_wrapped
        except Exception:
            pass

        if wrapped_path and os.path.exists(wrapped_path):
            try:
                with open(wrapped_path, "r") as f:
                    wrapped_content = f.read()

                match_list = re.search(r'\[(.*?)\]', wrapped_content)
                if match_list:
                    list_content = match_list.group(1)
                    paths = re.findall(r"['\"]([^'\"]+)['\"]", list_content)
                    for p in paths:
                        if os.path.exists(p):
                            site.addsitedir(p)
            except Exception:
                pass

    home_dir = os.path.expanduser("~")
    local_candidates = [
        os.path.join(home_dir, "tree-sitter-crystal", "bindings", "python"),
        os.path.join(os.getcwd(), "tree-sitter-crystal", "bindings", "python"),
    ]
    for candidate in local_candidates:
        if os.path.exists(candidate):
            site.addsitedir(candidate)

    try:
        import tree_sitter_crystal
    except ImportError:
        nix_store = "/nix/store"
        if os.path.exists(nix_store):
            # Look for site-packages containing tree_sitter_crystal
            py_ver = f"python{sys.version_info.major}.{sys.version_info.minor}"
            patterns = [
                os.path.join(nix_store, f"*python*-python-tree-sitter-crystal*/lib/{py_ver}/site-packages"),
                os.path.join(nix_store, f"*python*-python-tree-sitter-crystal*/lib/python*/site-packages"),
                os.path.join(nix_store, f"*tree-sitter-crystal*/lib/{py_ver}/site-packages"),
                os.path.join(nix_store, f"*tree-sitter-crystal*/lib/python*/site-packages"),
                # Search inside .venv site-packages of cached Nix builds
                os.path.join(nix_store, "*/.venv/lib/*/site-packages"),
            ]
            for pattern in patterns:
                for p in glob.glob(pattern):
                    if os.path.exists(p):
                        site.addsitedir(p)

setup_paths()

try:
    import graphify.detect
    import graphify.extract
except ImportError:
    print("Error: graphify is not installed or could not be found in python packages.", file=sys.stderr)
    sys.exit(1)

graphify.detect.CODE_EXTENSIONS.add('.cr')
graphify.detect.CODE_EXTENSIONS.add('.inc')

_orig_is_sensitive = graphify.detect._is_sensitive
def _custom_is_sensitive(path):
    if path.suffix in {'.cr', '.inc'}:
        return False
    return _orig_is_sensitive(path)
graphify.detect._is_sensitive = _custom_is_sensitive

from graphify.extract import LanguageConfig, _extract_generic
_CRYSTAL_CONFIG = LanguageConfig(
    ts_module="tree_sitter_crystal",
    class_types=frozenset({"class_def", "module_def", "struct_def", "alias_def"}),
    function_types=frozenset({"method_def"}),
    import_types=frozenset(),
    call_types=frozenset({"call"}),
    call_function_field="",
    call_accessor_node_types=frozenset(),
    name_fallback_child_types=("constant", "identifier"),
    body_fallback_child_types=("expressions",),
    function_boundary_types=frozenset({"method_def"}),
)

def extract_crystal(path):
    return _extract_generic(path, _CRYSTAL_CONFIG)

def patched_extract(paths: list[Path], cache_root: Path | None = None) -> dict:
    from graphify.extract import (
        _check_tree_sitter_version, load_cached, save_cached,
        _resolve_cross_file_imports, extract_blade,
        extract_python, extract_js, extract_go, extract_rust,
        extract_java, extract_c, extract_cpp, extract_ruby,
        extract_csharp, extract_kotlin, extract_scala, extract_php,
        extract_swift, extract_lua, extract_zig, extract_powershell,
        extract_elixir, extract_objc, extract_julia, extract_dart,
        extract_verilog
    )

    _check_tree_sitter_version()
    per_file: list[dict] = []

    try:
        if not paths:
            root = Path(".")
        elif len(paths) == 1:
            root = paths[0].parent
        else:
            common_len = sum(
                1 for i in range(min(len(p.parts) for p in paths))
                if len({p.parts[i] for p in paths}) == 1
            )
            root = Path(*paths[0].parts[:common_len]) if common_len else Path(".")
    except Exception:
        root = Path(".")

    _DISPATCH = {
        ".py": extract_python,
        ".js": extract_js, ".jsx": extract_js, ".mjs": extract_js,
        ".ts": extract_js, ".tsx": extract_js,
        ".go": extract_go,
        ".rs": extract_rust,
        ".java": extract_java,
        ".c": extract_c, ".h": extract_c, ".inc": extract_c,
        ".cpp": extract_cpp, ".cc": extract_cpp, ".cxx": extract_cpp, ".hpp": extract_cpp,
        ".rb": extract_ruby,
        ".cs": extract_csharp,
        ".kt": extract_kotlin, ".kts": extract_kotlin,
        ".scala": extract_scala,
        ".php": extract_php,
        ".swift": extract_swift,
        ".lua": extract_lua, ".toc": extract_lua,
        ".zig": extract_zig,
        ".ps1": extract_powershell,
        ".ex": extract_elixir, ".exs": extract_elixir,
        ".m": extract_objc, ".mm": extract_objc,
        ".jl": extract_julia,
        ".vue": extract_js, ".svelte": extract_js,
        ".dart": extract_dart,
        ".v": extract_verilog, ".sv": extract_verilog,
        ".cr": extract_crystal,
    }

    total = len(paths)
    _PROGRESS_INTERVAL = 50
    for i, path in enumerate(paths):
        if total >= _PROGRESS_INTERVAL and i % _PROGRESS_INTERVAL == 0 and i > 0:
            print(f"  AST extraction: {i}/{total} files ({i * 100 // total}%)", flush=True)
        if path.name.endswith(".blade.php"):
            extractor = extract_blade
        else:
            extractor = _DISPATCH.get(path.suffix)
        if extractor is None:
            continue
        cached = load_cached(path, cache_root or root)
        if cached is not None:
            per_file.append(cached)
            continue
        result = extractor(path)
        if "error" not in result:
            save_cached(path, result, cache_root or root)
        per_file.append(result)
    if total >= _PROGRESS_INTERVAL:
        print(f"  AST extraction: {total}/{total} files (100%)", flush=True)

    all_nodes: list[dict] = []
    all_edges: list[dict] = []
    for result in per_file:
        all_nodes.extend(result.get("nodes", []))
        all_edges.extend(result.get("edges", []))

    py_paths = [p for p in paths if p.suffix == ".py"]
    if py_paths:
        py_results = [r for r, p in zip(per_file, paths) if p.suffix == ".py"]
        try:
            cross_file_edges = _resolve_cross_file_imports(py_results, py_paths)
            all_edges.extend(cross_file_edges)
        except Exception as exc:
            import logging
            logging.getLogger(__name__).warning("Cross-file import resolution failed: %s", exc)

    global_label_to_nid: dict[str, str] = {}
    for n in all_nodes:
        raw = n.get("label", "")
        normalised = raw.strip("()").lstrip(".")
        if normalised:
            global_label_to_nid[normalised.lower()] = n["id"]

    existing_pairs = {(e["source"], e["target"]) for e in all_edges}
    for result in per_file:
        for rc in result.get("raw_calls", []):
            callee = rc.get("callee", "")
            if not callee:
                continue
            tgt = global_label_to_nid.get(callee.lower())
            caller = rc["caller_nid"]
            if tgt and tgt != caller and (caller, tgt) not in existing_pairs:
                existing_pairs.add((caller, tgt))
                all_edges.append({
                    "source": caller,
                    "target": tgt,
                    "relation": "calls",
                    "confidence": "INFERRED",
                    "confidence_score": 0.8,
                    "source_file": rc.get("source_file", ""),
                    "source_location": rc.get("source_location"),
                    "weight": 1.0,
                })

    return {
        "nodes": all_nodes,
        "edges": all_edges,
        "input_tokens": 0,
        "output_tokens": 0,
    }

graphify.extract.extract = patched_extract

def collect_files(targets: list[Path], excludes: list[Path]) -> list[Path]:
    collected = []
    dir_extensions = {
        "Volt": {".cr"},
        "C": {".c", ".h", ".inc"},
    }

    resolved_excludes = [ex.resolve() for ex in excludes]

    for target in targets:
        target_resolved = target.resolve()
        if not target_resolved.exists():
            print(f"[WARN] Target path not found: {target}", flush=True)
            continue

        if target_resolved.is_file():
            is_excluded = any(str(target_resolved).startswith(str(ex)) for ex in resolved_excludes)
            if not is_excluded:
                collected.append(target_resolved)
            continue

        exts = set()
        for key, val in dir_extensions.items():
            if key.lower() in target_resolved.name.lower() or key.lower() in [p.lower() for p in target_resolved.parts]:
                exts.update(val)

        if not exts:
            exts = graphify.detect.CODE_EXTENSIONS

        for path in sorted(target_resolved.rglob("*")):
            if not path.is_file():
                continue
            path_resolved = path.resolve()
            is_excluded = any(str(path_resolved).startswith(str(ex)) for ex in resolved_excludes)
            if is_excluded:
                continue
            if path_resolved.suffix in exts:
                collected.append(path_resolved)

    return collected


def run_build(targets: list[Path], excludes: list[Path], out_dir: Path):
    from graphify.build import build_from_json
    from graphify.cluster import cluster, score_all
    from graphify.analyze import god_nodes, surprising_connections, suggest_questions
    from graphify.report import generate
    from graphify.export import to_json, to_html

    files = collect_files(targets, excludes)

    print(f"\n[graphify] Scanning {len(files)} source files...", flush=True)
    ext_breakdown = {}
    for f in files:
        ext_breakdown[f.suffix] = ext_breakdown.get(f.suffix, 0) + 1
    for ext, count in sorted(ext_breakdown.items()):
        print(f"  Extension {ext}: {count} files", flush=True)
    print(flush=True)

    out_dir.mkdir(exist_ok=True, parents=True)

    t0 = time.time()
    print("[graphify] Phase 1: AST extraction...", flush=True)
    extraction = patched_extract(files, cache_root=out_dir)
    t1 = time.time()
    print(f"[graphify] Extraction finished in {t1-t0:.1f}s "
          f"({len(extraction['nodes'])} nodes, {len(extraction['edges'])} edges)", flush=True)

    print("[graphify] Phase 2: Building network graph...", flush=True)
    G = build_from_json(extraction)
    t2 = time.time()
    print(f"[graphify] Graph built in {t2-t1:.1f}s "
          f"({G.number_of_nodes()} nodes, {G.number_of_edges()} edges)", flush=True)

    print("[graphify] Phase 3: Clustering communities...", flush=True)
    communities = cluster(G)
    cohesion = score_all(G, communities)
    t3 = time.time()
    print(f"[graphify] Clustering finished in {t3-t2:.1f}s "
          f"({len(communities)} communities)", flush=True)

    print("[graphify] Phase 4: Analyzing nodes & producing report...", flush=True)
    gods = god_nodes(G)
    surprises = surprising_connections(G, communities)
    labels = {cid: f"Community {cid}" for cid in communities}
    questions = suggest_questions(G, communities, labels)

    detection = {
        "files": {
            "code": [str(f) for f in files],
            "document": [], "paper": [], "image": [],
        },
        "total_files": len(files),
        "total_words": 0,
    }

    report = generate(
        G, communities, cohesion, labels, gods, surprises,
        detection, {"input": 0, "output": 0}, str(out_dir.parent),
        suggested_questions=questions
    )
    (out_dir / "GRAPH_REPORT.md").write_text(report, encoding="utf-8")
    to_json(G, communities, str(out_dir / "graph.json"))

    html_written = False
    try:
        to_html(G, communities, str(out_dir / "graph.html"), community_labels=labels or None)
        html_written = True
    except ValueError as viz_err:
        print(f"[graphify] Skipped graph.html visualization creation: {viz_err}", flush=True)
        stale = out_dir / "graph.html"
        if stale.exists():
            stale.unlink()

    t4 = time.time()
    products = "graph.json" + (", graph.html" if html_written else "") + ", GRAPH_REPORT.md"
    print(f"\n[graphify] Completed successfully in {t4-t0:.1f}s total.", flush=True)
    print(f"[graphify] Outputs: {products}", flush=True)
    print(f"[graphify] Location: {out_dir}", flush=True)

def main():
    parser = argparse.ArgumentParser(description="Clean, modular Graphify CLI with custom tree-sitter support.")
    parser.add_argument("targets", nargs="*", type=Path, help="Target paths to scan.")
    parser.add_argument("--exclude", nargs="+", type=Path, default=[], help="Exclude sub-paths/folders from the scan.")
    parser.add_argument("--output-dir", type=Path, default=Path("graphify-out"), help="Directory where graphify outputs will be saved.")
    args = parser.parse_args()

    targets = args.targets
    if not targets:
        targets = [Path("Source/Volt"), Path("Source/C")]

    excludes = args.exclude
    if not excludes:
        excludes = [Path("Benchmarks"), Path("Core")]

    run_build(targets, excludes, args.output_dir)

if __name__ == "__main__":
    main()
