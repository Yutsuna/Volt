backendcore_root = meson.current_source_dir()
backendcore_found = run_command(
    find_prog,
    backendcore_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
backendcore_sources = []
foreach p : backendcore_found
    backendcore_sources += [p.replace(backendcore_root + '/', '')]
endforeach

backendcore_export = configure_file(
    input: export_template,
    output: 'BackendCore_export.hpp',
    configuration: {'MODULE': 'BACKENDCORE'},
)

backendcore_lib = library(
    'BackendCore',
    files(backendcore_sources),
    backendcore_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public', 'Private'),
    dependencies: [compile_options_dep, volt_middleend_dep],
    cpp_args: build_shared ? ['-DBACKENDCORE_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

backendcore_dep = declare_dependency(
    link_with: backendcore_lib,
    include_directories: [
        include_directories('Public'),
        include_directories('.'),
    ],
    dependencies: volt_middleend_dep,
)
