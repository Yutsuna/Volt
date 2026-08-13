volt_middleend_typesystem_inc = [
    include_directories('Public'),
    include_directories('.'),
    include_directories('../IR/Public'),
    include_directories('../IR'),
]

volt_middleend_typesystem_export = configure_file(
    input: export_template,
    output: 'VoltMiddleEndTypeSystem_export.hpp',
    configuration: {'MODULE': 'VOLT_MIDDLEEND_TYPESYSTEM'},
)

volt_middleend_typesystem_lib = static_library(
    'VoltMiddleEndTypeSystem',
    files(
        'Private/TypeStoreSerialize.cpp',
        'Private/SemaTypeSerialize.cpp',
        'Private/TypeResolve.cpp',
        'Private/TypeCompat.cpp',
        'Private/Reinstantiate.cpp',
        'Private/SelfCheck.cpp',
    ),
    volt_middleend_typesystem_export,
    cpp_pch: common_pch_header,
    include_directories: [
        volt_middleend_typesystem_inc,
        # Private-only, compile-time header visibility for Reinstantiate.cpp:
        # ReinstantiateBody only forward-declares Resolver::ScopeTable in the
        # public header (Instantiate.hpp) to keep TypeSystem's public API
        # DAG-clean, but its .cpp needs the complete type. No link dependency
        # on Resolver follows from this — ScopeTable's methods are header-inline.
        include_directories('../Resolver/Public'),
        include_directories('../Resolver'),
        # Reinstantiate.cpp still drives the not-yet-migrated Analysis-tier
        # TypeChecker machinery (TypeCheckerContext, ClosureInferencer,
        # LiteralInferencer, Lifetime/Temporaries) directly from
        # source/Volt/Sema — that migration is Etape 8, out of this task's
        # scope, and the old locations are the only ones that exist today.
        include_directories('../../Sema/Public'),
        include_directories('../../Sema/Private'),
        include_directories('../../Sema/Private/Passes/TypeChecker'),
        include_directories('../../Sema'),
    ],
    dependencies: [
        compile_options_dep,
        volt_middleend_core_dep,
        volt_middleend_ir_dep,
        core_dep,
        frontend_dep,
    ],
    cpp_args: build_shared ? ['-DVOLT_MIDDLEEND_TYPESYSTEM_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

volt_middleend_typesystem_dep = declare_dependency(
    link_with: volt_middleend_typesystem_lib,
    include_directories: volt_middleend_typesystem_inc,
    dependencies: [
        volt_middleend_core_dep,
        volt_middleend_ir_dep,
        core_dep,
        frontend_dep,
    ],
)
