driver_root = meson.current_source_dir()
driver_found = run_command(
    find_prog,
    driver_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
driver_sources = []
foreach p : driver_found
    driver_sources += [p.replace(driver_root + '/', '')]
endforeach

driver_export = configure_file(
    input: export_template,
    output: 'Driver_export.hpp',
    configuration: {'MODULE': 'DRIVER'},
)

driver_deps = [compile_options_dep, volt_middleend_dep, backendcore_dep]
driver_args = [
    '-DVOLT_DEV_STDLIB_DIR="' + meson.project_source_root() / 'source' / 'Lib' + '"',
    '-DVOLT_DEV_REPL_PRELUDE_DIR="'
    + meson.project_source_root() / 'source' / 'Volt' / 'REPL' / 'Prelude'
    + '"',
]
driver_args += build_shared ? ['-DDRIVER_BUILD_SHARED'] : []
if get_option('enable_llvm')
    driver_deps += [llvm_dep, backendllvm_dep]
    driver_args += ['-DVOLT_ENABLE_LLVM']
endif
if jit_enabled
    driver_deps += [backendjit_dep]
    driver_args += ['-DVOLT_ENABLE_JIT']
endif

driver_lib = library(
    'Driver',
    files(driver_sources),
    driver_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: driver_deps,
    cpp_args: driver_args,
    gnu_symbol_visibility: 'hidden',
    install: true,
)

driver_link_deps = [volt_middleend_dep, backendcore_dep]
if get_option('enable_llvm')
    driver_link_deps += [backendllvm_dep]
endif
if jit_enabled
    driver_link_deps += [backendjit_dep]
endif

driver_dep = declare_dependency(
    link_with: driver_lib,
    include_directories: [
        include_directories('Public'),
        include_directories('.'),
    ],
    dependencies: driver_link_deps,
)
