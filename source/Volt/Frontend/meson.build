frontend_root = meson.current_source_dir()
frontend_found = run_command(
    find_prog,
    frontend_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
frontend_sources = []
foreach p : frontend_found
    frontend_sources += [p.replace(frontend_root + '/', '')]
endforeach

frontend_export = configure_file(
    input: export_template,
    output: 'Frontend_export.hpp',
    configuration: {'MODULE': 'FRONTEND'},
)

frontend_lib = library(
    'Frontend',
    files(frontend_sources),
    frontend_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: [compile_options_dep, core_dep],
    cpp_args: build_shared ? ['-DFRONTEND_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

frontend_dep = declare_dependency(
    link_with: frontend_lib,
    include_directories: [
        include_directories('Public'),
        include_directories('.'),
    ],
    dependencies: core_dep,
)
