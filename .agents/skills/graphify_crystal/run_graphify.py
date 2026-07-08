import sys
import site
import re

# Add Nix store paths so that tree-sitter, tree-sitter-crystal, etc. are found
paths = [
    '/nix/store/vi82955qwvyva44qpblmman2154cn2lp-graphify-latest/lib/python3.13/site-packages',
    '/nix/store/5yr31fd6xyci9xbxywh7f7bb7cyszjd5-python3.13-networkx-3.6.1/lib/python3.13/site-packages',
    '/nix/store/jicdqi2cq2bp8bm9pwk35mq521dl7n4l-python3.13-datasketch-1.10.0/lib/python3.13/site-packages',
    '/nix/store/9asvh5g5wxj0aywr5f9wbn2ib27pfkkb-python3.13-numpy-2.4.4/lib/python3.13/site-packages',
    '/nix/store/m0v936rc6b8lckpi7xgsbhbmgis5hy95-python3.13-scipy-1.17.1/lib/python3.13/site-packages',
    '/nix/store/ygxvx1f74mqmd2q5cdrqlmlm0j9lz8lc-python3.13-rapidfuzz-3.14.5/lib/python3.13/site-packages',
    '/nix/store/b4m8560740006vfnx3gps7dd6cm7zxiz-python3.13-tree-sitter-0.25.2/lib/python3.13/site-packages',
    '/nix/store/jmwx5j10n7qh3c3fdqzjmi1gmjb8lbaw-python3.13-python-tree-sitter-python-0.25.0/lib/python3.13/site-packages',
    '/nix/store/8ry246ipmixw8fhpq5f7h72bq1daj2fg-python3.13-tree-sitter-config-1.0.0/lib/python3.13/site-packages',
    '/nix/store/zs9s5hdr7ibb9rfx0bv35b38x51vnb4a-python3.13-pydantic-2.12.5/lib/python3.13/site-packages',
    '/nix/store/2pplli407gv19zabvic99sz5pawiq1pp-python3.13-annotated-types-0.7.0/lib/python3.13/site-packages',
    '/nix/store/y5bb4gli12v0pvq1rck01w4zmdavfayz-python3.13-pydantic-core-2.41.5/lib/python3.13/site-packages',
    '/nix/store/24lnp6kjbxwj3lw5a1x65qipmmjp04wj-python3.13-typing-extensions-4.15.0/lib/python3.13/site-packages',
    '/nix/store/pvvkmvv42s7kiwgymhyl4im853y150ky-python3.13-typing-inspection-0.4.2/lib/python3.13/site-packages',
    '/nix/store/3rbwfnqy3j54826c5pi2chqcjhsj5qw9-python3.13-email-validator-2.3.0/lib/python3.13/site-packages',
    '/nix/store/p92gcxfkkzpsxk3x0nhh541ri0c21iil-python3.13-dnspython-2.8.0/lib/python3.13/site-packages',
    '/nix/store/l600sbvlb7px0dr040ff47glf8d502zb-python3.13-idna-3.13/lib/python3.13/site-packages',
    '/nix/store/kfc2pwm3ba9h8ksjxynvs4l8lrsdfbwh-python3.13-python-tree-sitter-javascript-0.25.0/lib/python3.13/site-packages',
    '/nix/store/4p66amg87hda7hmls6awd6hhgx2m65ni-python3.13-python-tree-sitter-typescript-0.23.2/lib/python3.13/site-packages',
    '/nix/store/g4mk45xlx5z8fspail8kfign9nw64qy4-python3.13-python-tree-sitter-go-0.25.0/lib/python3.13/site-packages',
    '/nix/store/sdkqp9lnkfbkx4vfxi5pnw70s0zc187x-python3.13-python-tree-sitter-rust-0.24.2/lib/python3.13/site-packages',
    '/nix/store/khskdws0l4x3rp8i9xkyld7ffriwy326-python3.13-python-tree-sitter-java-0.23.5/lib/python3.13/site-packages',
    '/nix/store/srhssh1dm91kzzmfya8g4jc6q1y9nzda-python3.13-python-tree-sitter-c-0.24.2/lib/python3.13/site-packages',
    '/nix/store/gl6qa8ify7pjs9wj7h7f2ly5k0vkbp51-python3.13-python-tree-sitter-cpp-0.23.4/lib/python3.13/site-packages',
    '/nix/store/vql1k1dwwgvcnq4y252zs267p1hs8rm6-python3.13-python-tree-sitter-ruby-0.23.1/lib/python3.13/site-packages',
    '/nix/store/aws40flfjdq2ncwp50fylzbbzg4ysv86-python3.13-python-tree-sitter-c-sharp-0.23.5/lib/python3.13/site-packages',
    '/nix/store/vmpm28ygv0v8vbz29j4aw0rizbhv9x12-python3.13-python-tree-sitter-kotlin-0.3.8/lib/python3.13/site-packages',
    '/nix/store/bkinv5fki20j2zjfxb3m243jdp84mil3-python3.13-python-tree-sitter-scala-0.26.0/lib/python3.13/site-packages',
    '/nix/store/n3zbnga6jmrxmbah1vm8r03ccdxmc7xp-python3.13-python-tree-sitter-php-0.24.2/lib/python3.13/site-packages',
    '/nix/store/ps4n8kgqj5bgrwnf0sliy7nkgvx2cj1h-python3.13-python-tree-sitter-swift-0.7.3/lib/python3.13/site-packages',
    '/nix/store/fj71vsb0qpbk7l1kxdj1py19bqaidjgv-python3.13-python-tree-sitter-lua-0.0.19+unstable20260226/lib/python3.13/site-packages',
    '/nix/store/rhdvljm6g8j05q663rf8n8bv14vmdxmd-python3.13-python-tree-sitter-zig-1.1.2+unstable20250910/lib/python3.13/site-packages',
    '/nix/store/wv06q8rh4wgpck7piamdmp5x2zfc2h7v-python3.13-python-tree-sitter-elixir-0.3.5/lib/python3.13/site-packages',
    '/nix/store/q6rv4fhlbrg84vkjzgp8lg509s382w9j-python3.13-python-tree-sitter-objc-3.0.2/lib/python3.13/site-packages',
    '/nix/store/ibiv6mm8fw1608gs6yyzaj0qcs2hpx5b-python3.13-python-tree-sitter-julia-0.25.0/lib/python3.13/site-packages',
    '/nix/store/q6jgixzklvsf0nk5rq7w8gj6g8vprgqq-python3.13-python-tree-sitter-verilog-1.0.3/lib/python3.13/site-packages',
    '/nix/store/qrfckn52xhsrih8ynis2v3q63mk8g2ak-python3.13-python-tree-sitter-fortran-0.5.1/lib/python3.13/site-packages',
    '/nix/store/ga5c52bgil359dv7dyyawnnpga6znlfi-python3.13-python-tree-sitter-bash-0.25.1/lib/python3.13/site-packages',
    '/nix/store/q7nim3bfm9wb40nknhvbi0pm0zh8n75s-python3.13-python-tree-sitter-json-0.24.8/lib/python3.13/site-packages',
    '/nix/store/5q1zqb2v43rn7ck8lzvi1g3ps8czjsri-python3.13-python-tree-sitter-crystal-0+unstable20251012/lib/python3.13/site-packages'
]
for p in paths:
    site.addsitedir(p)

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
