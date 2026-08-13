volt_middleend_ir_inc = [
    include_directories('Public'),
    include_directories('.'),
]

volt_middleend_ir_export = configure_file(
    input: export_template,
    output: 'VoltMiddleEndIR_export.hpp',
    configuration: {'MODULE': 'VOLT_MIDDLEEND_IR'},
)

volt_middleend_ir_lib = static_library(
    'VoltMiddleEndIR',
    files(
        'Private/CalleeMapSerialize.cpp',
    ),
    volt_middleend_ir_export,
    cpp_pch: common_pch_header,
    include_directories: [
        volt_middleend_ir_inc,
        include_directories('../TypeSystem/Public'),
        include_directories('../TypeSystem'),
    ],
    dependencies: [
        compile_options_dep,
        volt_middleend_core_dep,
        core_dep,
        frontend_dep,
    ],
    cpp_args: build_shared ? ['-DVOLT_MIDDLEEND_IR_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

volt_middleend_ir_dep = declare_dependency(
    link_with: volt_middleend_ir_lib,
    include_directories: volt_middleend_ir_inc,
    dependencies: [
        volt_middleend_core_dep,
        core_dep,
        frontend_dep,
    ],
)
