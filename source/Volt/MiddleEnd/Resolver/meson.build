volt_middleend_resolver_inc = [
    include_directories('Public'),
    include_directories('.'),
    include_directories('../TypeSystem/Public'),
    include_directories('../TypeSystem'),
]

volt_middleend_resolver_export = configure_file(
    input: export_template,
    output: 'VoltMiddleEndResolver_export.hpp',
    configuration: {'MODULE': 'VOLT_MIDDLEEND_RESOLVER'},
)

volt_middleend_resolver_lib = static_library(
    'VoltMiddleEndResolver',
    files(
        'Private/TypeBinder.cpp',
        'Private/ScopeTableSerialize.cpp',
        'Private/InterfaceRegistry.cpp',
        'Private/ClosureFrame.cpp',
        'Private/ScopeResolver.cpp',
        'Private/SelfCheck.cpp',
    ),
    volt_middleend_resolver_export,
    cpp_pch: common_pch_header,
    include_directories: [
        volt_middleend_resolver_inc,
        include_directories('../../Sema/Public'),
        include_directories('../../Sema/Private/Passes/TypeChecker'),
        include_directories('../../Sema/Private'),
        include_directories('../../Sema'),
    ],
    dependencies: [
        compile_options_dep,
        volt_middleend_core_dep,
        volt_middleend_typesystem_dep,
        core_dep,
        frontend_dep,
    ],
    cpp_args: build_shared ? ['-DVOLT_MIDDLEEND_RESOLVER_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

volt_middleend_resolver_dep = declare_dependency(
    link_with: volt_middleend_resolver_lib,
    include_directories: volt_middleend_resolver_inc,
    dependencies: [
        volt_middleend_core_dep,
        volt_middleend_typesystem_dep,
        core_dep,
        frontend_dep,
    ],
)
