volt_middleend_optimisations_inc = [
    include_directories('Public'),
    include_directories('.'),
]

volt_middleend_optimisations_export = configure_file(
    input: export_template,
    output: 'VoltMiddleEndOptimisations_export.hpp',
    configuration: {'MODULE': 'VOLT_MIDDLEEND_OPTIMISATIONS'},
)

volt_middleend_optimisations_lib = static_library(
    'VoltMiddleEndOptimisations',
    files(
        'Private/BlockInliner.cpp',
        'Private/InlineSummary.cpp',
        'Private/SelfCheck.cpp',
    ),
    volt_middleend_optimisations_export,
    cpp_pch: common_pch_header,
    include_directories: [
        volt_middleend_optimisations_inc,
        include_directories('../Analysis/Public'),
        include_directories('../Analysis/Private'),
        include_directories('../Analysis'),
        include_directories('../Lowering/Public'),
        include_directories('../Lowering'),
    ],
    dependencies: [
        compile_options_dep,
        volt_middleend_core_dep,
        volt_middleend_ir_dep,
        volt_middleend_typesystem_dep,
        volt_middleend_resolver_dep,
        volt_middleend_consteval_dep,
        core_dep,
        frontend_dep,
    ],
    cpp_args: build_shared ? ['-DVOLT_MIDDLEEND_OPTIMISATIONS_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

volt_middleend_optimisations_dep = declare_dependency(
    link_with: volt_middleend_optimisations_lib,
    include_directories: volt_middleend_optimisations_inc,
    dependencies: [
        volt_middleend_core_dep,
        volt_middleend_ir_dep,
        volt_middleend_typesystem_dep,
        volt_middleend_resolver_dep,
        volt_middleend_consteval_dep,
        core_dep,
        frontend_dep,
    ],
)
