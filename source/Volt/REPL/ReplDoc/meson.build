repldoc_root = meson.current_source_dir()
repldoc_found = run_command(
    find_prog,
    repldoc_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
repldoc_sources = []
foreach p : repldoc_found
    repldoc_sources += [p.replace(repldoc_root + '/', '')]
endforeach

repldoc_export = configure_file(
    input: export_template,
    output: 'ReplDoc_export.hpp',
    configuration: {'MODULE': 'REPLDOC'},
)

repldoc_lib = library(
    'ReplDoc',
    files(repldoc_sources),
    repldoc_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: [compile_options_dep],
    cpp_args: build_shared ? ['-DREPLDOC_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

repldoc_dep = declare_dependency(
    link_with: repldoc_lib,
    include_directories: [include_directories('Public'), include_directories('.')],
)
