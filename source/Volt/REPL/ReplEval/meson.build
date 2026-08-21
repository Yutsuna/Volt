repleval_root = meson.current_source_dir()
repleval_found = run_command(
    find_prog,
    repleval_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
repleval_sources = []
foreach p : repleval_found
    repleval_sources += [p.replace(repleval_root + '/', '')]
endforeach

repleval_export = configure_file(
    input: export_template,
    output: 'ReplEval_export.hpp',
    configuration: {'MODULE': 'REPLEVAL'},
)

repleval_lib = library(
    'ReplEval',
    files(repleval_sources),
    repleval_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: [compile_options_dep, driver_dep],
    cpp_args: build_shared ? ['-DREPLEVAL_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

repleval_dep = declare_dependency(
    link_with: repleval_lib,
    include_directories: [include_directories('Public'), include_directories('.')],
    dependencies: [driver_dep],
)
