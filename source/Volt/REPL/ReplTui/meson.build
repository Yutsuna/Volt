repltui_root = meson.current_source_dir()
repltui_found = run_command(
    find_prog,
    repltui_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
repltui_sources = []
foreach p : repltui_found
    repltui_sources += [p.replace(repltui_root + '/', '')]
endforeach

repltui_export = configure_file(
    input: export_template,
    output: 'ReplTui_export.hpp',
    configuration: {'MODULE': 'REPLTUI'},
)

# The only module in REPL/ that is allowed <iostream>, ::write, termios or an
# escape sequence. tests/repl/Purity fails the build's own conformance check on
# any of those appearing anywhere else under REPL/.
repltui_lib = library(
    'ReplTui',
    files(repltui_sources),
    repltui_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public', 'Private'),
    dependencies: [
        compile_options_dep,
        repldoc_dep,
        replsyntax_dep,
        repleval_dep,
        replquery_dep,
        replcomplete_dep,
        replcore_dep,
    ],
    cpp_args: build_shared ? ['-DREPLTUI_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

repltui_dep = declare_dependency(
    link_with: repltui_lib,
    include_directories: [include_directories('Public'), include_directories('.')],
    dependencies: [repldoc_dep, replsyntax_dep, repleval_dep, replquery_dep, replcomplete_dep, replcore_dep],
)
