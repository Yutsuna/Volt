volt_middleend_consteval_inc = [
    include_directories('Public'),
    include_directories('.'),
]

volt_middleend_consteval_export = configure_file(
    input: export_template,
    output: 'VoltMiddleEndConstEval_export.hpp',
    configuration: {'MODULE': 'VOLT_MIDDLEEND_CONSTEVAL'},
)

volt_middleend_consteval_lib = static_library(
    'VoltMiddleEndConstEval',
    files(
        'Private/MagicExpansion.cpp',
        'Private/MacroExpansion.cpp',
        'Private/SelfCheck.cpp',
        'Private/TraitEngine.cpp',
    ),
    volt_middleend_consteval_export,
    cpp_pch: common_pch_header,
    include_directories: [
        volt_middleend_consteval_inc,
    ],
    dependencies: [
        compile_options_dep,
        volt_middleend_core_dep,
        # TraitEngine answers its questions off the TypeStore's own structure
        # (Includes / Super / Members), so it needs the type system proper —
        # which sits below ConstEval in §1's DAG, so this is an ordinary
        # forward edge, not the back-edge Lowering carries.
        volt_middleend_typesystem_dep,
        core_dep,
        frontend_dep,
    ],
    cpp_args: build_shared ? ['-DVOLT_MIDDLEEND_CONSTEVAL_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

volt_middleend_consteval_dep = declare_dependency(
    link_with: volt_middleend_consteval_lib,
    include_directories: volt_middleend_consteval_inc,
    dependencies: [
        volt_middleend_core_dep,
        core_dep,
        frontend_dep,
    ],
)
