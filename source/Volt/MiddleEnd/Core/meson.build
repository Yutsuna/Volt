volt_middleend_core_inc = [
    include_directories('Public'),
    include_directories('.'),
]

volt_middleend_core_export = configure_file(
    input: export_template,
    output: 'VoltMiddleEndCore_export.hpp',
    configuration: {'MODULE': 'VOLT_MIDDLEEND_CORE'},
)

volt_middleend_core_lib = static_library(
    'VoltMiddleEndCore',
    files(
        'Private/PassRegistry.cpp',
        'Private/SelfCheck.cpp',
    ),
    volt_middleend_core_export,
    cpp_pch: common_pch_header,
    include_directories: volt_middleend_core_inc,
    dependencies: [
        compile_options_dep,
        core_dep,
        frontend_dep,
    ],
    cpp_args: build_shared ? ['-DVOLT_MIDDLEEND_CORE_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

volt_middleend_core_dep = declare_dependency(
    link_with: volt_middleend_core_lib,
    include_directories: volt_middleend_core_inc,
    dependencies: [
        core_dep,
        frontend_dep,
    ],
)
