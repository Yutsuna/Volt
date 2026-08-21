replcore_root = meson.current_source_dir()
replcore_found = run_command(
    find_prog,
    replcore_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
replcore_sources = []
foreach p : replcore_found
    replcore_sources += [p.replace(replcore_root + '/', '')]
endforeach

replcore_export = configure_file(
    input: export_template,
    output: 'ReplCore_export.hpp',
    configuration: {'MODULE': 'REPLCORE'},
)

replcore_lib = library(
    'ReplCore',
    files(replcore_sources),
    replcore_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: [compile_options_dep, frontend_dep, repleval_dep],
    cpp_args: build_shared ? ['-DREPLCORE_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

replcore_dep = declare_dependency(
    link_with: replcore_lib,
    include_directories: [include_directories('Public'), include_directories('.')],
    dependencies: [frontend_dep, repleval_dep],
)
