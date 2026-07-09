import sys
import site
import os
import shutil
import re
import glob
from pathlib import Path

def setup_paths():
    # If graphify is already available, do nothing
    try:
        import graphify
        return
    except ImportError:
        pass

    # Try to find 'graphify' executable to extract Nix paths if in a Nix environment
    graphify_bin = shutil.which("graphify")
    if not graphify_bin:
        return

    real_graphify = os.path.realpath(graphify_bin)
    if not os.path.exists(real_graphify):
        return

    # Check if the binary or a .graphify-wrapped adjacent to it/referenced in it has the paths
    wrapped_path = None
    try:
        with open(real_graphify, "r") as f:
            wrapper_content = f.read(4096)

        # Look for the wrapped script pattern
        match_wrapped = re.search(r'exec -a "\$0"\s+"([^"]+\.graphify-wrapped)"', wrapper_content)
        if match_wrapped:
            wrapped_path = match_wrapped.group(1)
        else:
            potential_wrapped = os.path.join(os.path.dirname(real_graphify), ".graphify-wrapped")
            if os.path.exists(potential_wrapped):
                wrapped_path = potential_wrapped
    except Exception:
        pass

    if not wrapped_path or not os.path.exists(wrapped_path):
        return

    try:
        with open(wrapped_path, "r") as f:
            wrapped_content = f.read()

        # Extract the list of dependency paths dynamically
        match_list = re.search(r'\[(.*?)\]', wrapped_content)
        if match_list:
            list_content = match_list.group(1)
            paths = re.findall(r"['\"]([^'\"]+)['\"]", list_content)
            for p in paths:
                if os.path.exists(p):
                    site.addsitedir(p)
    except Exception:
        pass

setup_paths()

# Try to find and add tree-sitter-crystal dynamically
try:
    import tree_sitter_crystal
except ImportError:
    graphify_bin = shutil.which("graphify")
    nix_store = "/nix/store"
    if graphify_bin:
        real_graphify = os.path.realpath(graphify_bin)
        for parent in Path(real_graphify).parents:
            if parent.name == "store" and parent.parent.name == "nix":
                nix_store = str(parent)
                break
    if os.path.exists(nix_store):
        py_ver = f"python{sys.version_info.major}.{sys.version_info.minor}"
        patterns = [
            os.path.join(nix_store, f"*python*-python-tree-sitter-crystal*/lib/{py_ver}/site-packages"),
            os.path.join(nix_store, f"*python*-python-tree-sitter-crystal*/lib/python*/site-packages"),
            os.path.join(nix_store, f"*tree-sitter-crystal*/lib/{py_ver}/site-packages"),
            os.path.join(nix_store, f"*tree-sitter-crystal*/lib/python*/site-packages"),
        ]
        for pattern in patterns:
            for p in glob.glob(pattern):
                if os.path.exists(p):
                    site.addsitedir(p)

import graphify.detect
import graphify.extract

# Patch CODE_EXTENSIONS to include .cr and .inc
graphify.detect.CODE_EXTENSIONS.add('.cr')
graphify.detect.CODE_EXTENSIONS.add('.inc')

# Override _is_sensitive to not skip .cr and .inc files (e.g. Token.cr, TokenSpec.cr)
_orig_is_sensitive = graphify.detect._is_sensitive
def _custom_is_sensitive(path):
    if path.suffix in {'.cr', '.inc'}:
        return False
    return _orig_is_sensitive(path)
graphify.detect._is_sensitive = _custom_is_sensitive

# Define Crystal config using tree-sitter-crystal
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

# Register Crystal config and mapping
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

    # Infer a common root for cache keys
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
        ".js": extract_js,
        ".jsx": extract_js,
        ".mjs": extract_js,
        ".ts": extract_js,
        ".tsx": extract_js,
        ".go": extract_go,
        ".rs": extract_rust,
        ".java": extract_java,
        ".c": extract_c,
        ".h": extract_c,
        ".inc": extract_c,
        ".cpp": extract_cpp,
        ".cc": extract_cpp,
        ".cxx": extract_cpp,
        ".hpp": extract_cpp,
        ".rb": extract_ruby,
        ".cs": extract_csharp,
        ".kt": extract_kotlin,
        ".kts": extract_kotlin,
        ".scala": extract_scala,
        ".php": extract_php,
        ".swift": extract_swift,
        ".lua": extract_lua,
        ".toc": extract_lua,
        ".zig": extract_zig,
        ".ps1": extract_powershell,
        ".ex": extract_elixir,
        ".exs": extract_elixir,
        ".m": extract_objc,
        ".mm": extract_objc,
        ".jl": extract_julia,
        ".vue": extract_js,
        ".svelte": extract_js,
        ".dart": extract_dart,
        ".v": extract_verilog,
        ".sv": extract_verilog,
        ".cr": extract_crystal,
    }

    total = len(paths)
    _PROGRESS_INTERVAL = 100
    for i, path in enumerate(paths):
        if total >= _PROGRESS_INTERVAL and i % _PROGRESS_INTERVAL == 0 and i > 0:
            print(f"  AST extraction: {i}/{total} files ({i * 100 // total}%)", flush=True)
        # .blade.php must be checked before suffix lookup since Path.suffix returns .php
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

    # Add cross-file class-level edges (Python only - uses Python parser internally)
    py_paths = [p for p in paths if p.suffix == ".py"]
    if py_paths:
        py_results = [r for r, p in zip(per_file, paths) if p.suffix == ".py"]
        try:
            cross_file_edges = _resolve_cross_file_imports(py_results, py_paths)
            all_edges.extend(cross_file_edges)
        except Exception as exc:
            import logging
            logging.getLogger(__name__).warning("Cross-file import resolution failed, skipping: %s", exc)

    # Cross-file call resolution for all languages
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

# Also run main CLI
from graphify.__main__ import main
if __name__ == "__main__":
    sys.argv[0] = re.sub(r"(-script\.pyw|\.exe)?$", "", sys.argv[0])
    main()
