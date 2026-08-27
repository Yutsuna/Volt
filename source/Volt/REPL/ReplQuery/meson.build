replquery_root = meson.current_source_dir()
replquery_found = run_command(
    find_prog,
    replquery_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
replquery_sources = []
foreach p : replquery_found
    replquery_sources += [p.replace(replquery_root + '/', '')]
endforeach

replquery_export = configure_file(
    input: export_template,
    output: 'ReplQuery_export.hpp',
    configuration: {'MODULE': 'REPLQUERY'},
)

# ReplEval for the facts, ReplDoc and ReplSyntax for the shapes they are shown
# in. No terminal and no backend header: a builtin returns a Document.
replquery_lib = library(
    'ReplQuery',
    files(replquery_sources),
    replquery_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: [compile_options_dep, repldoc_dep, replsyntax_dep, repleval_dep],
    cpp_args: build_shared ? ['-DREPLQUERY_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

replquery_dep = declare_dependency(
    link_with: replquery_lib,
    include_directories: [include_directories('Public'), include_directories('.')],
    dependencies: [repldoc_dep, replsyntax_dep, repleval_dep],
)
