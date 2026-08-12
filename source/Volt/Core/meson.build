core_root = meson.current_source_dir()
core_found = run_command(
    find_prog,
    core_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
core_sources = []
foreach p : core_found
    core_sources += [p.replace(core_root + '/', '')]
endforeach

core_export = configure_file(
    input: export_template,
    output: 'Core_export.hpp',
    configuration: {'MODULE': 'CORE'},
)

core_lib = library(
    'Core',
    files(core_sources),
    core_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: compile_options_dep,
    cpp_args: build_shared ? ['-DCORE_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

core_dep = declare_dependency(
    link_with: core_lib,
    include_directories: [
        include_directories('Public'),
        include_directories('.'),
    ],
)
