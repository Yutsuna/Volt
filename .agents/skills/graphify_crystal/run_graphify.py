import sys
import site
import os
import shutil
import re

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

        # Extract the list of dependency paths passed to addsitedir
        paths_match = re.search(r'\[\s*(\'/nix/store/[^\]]+)\s*\]', wrapped_content)
        if paths_match:
            paths_str = paths_match.group(1)
            paths = [p.strip().strip("'").strip('"') for p in paths_str.split(",")]
            for p in paths:
                if os.path.exists(p):
                    site.addsitedir(p)
    except Exception:
        pass

setup_paths()

import graphify.detect
import graphify.extract

# Patch CODE_EXTENSIONS to include .cr
graphify.detect.CODE_EXTENSIONS.add('.cr')

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

graphify.extract._DISPATCH['.cr'] = extract_crystal

# Also run main CLI
from graphify.__main__ import main
if __name__ == "__main__":
    sys.argv[0] = re.sub(r"(-script\.pyw|\.exe)?$", "", sys.argv[0])
    main()
