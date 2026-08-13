sema_root = meson.current_source_dir()
sema_found = run_command(
    find_prog,
    sema_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
sema_sources = []
foreach p : sema_found
    sema_sources += [p.replace(sema_root + '/', '')]
endforeach

sema_export = configure_file(
    input: export_template,
    output: 'Sema_export.hpp',
    configuration: {'MODULE': 'SEMA'},
)

sema_lib = library(
    'Sema',
    files(sema_sources),
    sema_export,
    cpp_pch: common_pch_header,
    include_directories: [
        include_directories('Public', 'Private'),
        volt_middleend_core_inc,
        volt_middleend_ir_inc,
        volt_middleend_typesystem_inc,
        volt_middleend_resolver_inc,
        volt_middleend_consteval_inc,
    ],
    dependencies: [compile_options_dep, frontend_dep, volt_middleend_dep],
    cpp_args: build_shared ? ['-DSEMA_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
    override_options: ['unity_size=8'],
)

sema_dep = declare_dependency(
    link_with: sema_lib,
    include_directories: [
        include_directories('Public'),
        include_directories('.'),
        volt_middleend_core_inc,
        volt_middleend_ir_inc,
        volt_middleend_typesystem_inc,
        volt_middleend_resolver_inc,
        volt_middleend_consteval_inc,
    ],
    dependencies: [frontend_dep, volt_middleend_dep],
)
