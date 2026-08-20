volt_middleend_analysis_inc = [
    include_directories('Public'),
    include_directories('Private'),
    include_directories('.'),
]

volt_middleend_analysis_export = configure_file(
    input: export_template,
    output: 'VoltMiddleEndAnalysis_export.hpp',
    configuration: {'MODULE': 'VOLT_MIDDLEEND_ANALYSIS'},
)

volt_middleend_analysis_lib = static_library(
    'VoltMiddleEndAnalysis',
    files(
        'Private/TypeChecker.cpp',
        'Private/TypeCheckerContext.cpp',
        'Private/TypeCheckerConstraint.cpp',
        'Private/ExprInferencer.cpp',
        'Private/DeclStmtWalker.cpp',
        'Private/MemberResolver.cpp',
        'Private/LiteralInferencer.cpp',
        'Private/ClosureInferencer.cpp',
        'Private/UnusedChecker.cpp',
        'Private/AstInvariant.cpp',
        'Private/InstantiationCache.cpp',
        'Private/Raii/Ownership.cpp',
        'Private/Raii/OwnershipInference.cpp',
        'Private/Lifetime/CleanupRegion.cpp',
        'Private/Lifetime/ExitPaths.cpp',
        'Private/Lifetime/FieldCascade.cpp',
        'Private/Lifetime/ScopeCleanup.cpp',
        'Private/Lifetime/Temporaries.cpp',
    ),
    volt_middleend_analysis_export,
    cpp_pch: common_pch_header,
    include_directories: [
        volt_middleend_analysis_inc,
        # TypeChecker.cpp drives the post-walk sweeps that live in Lowering
        # (LowerArrayLits, LowerEnumCases, LowerClosureLits,
        # InsertFinalizeCalls) and Optimisations (InlineBlockCalls).
        include_directories('../Lowering/Public'),
        include_directories('../Lowering'),
        include_directories('../Optimisations/Public'),
        include_directories('../Optimisations'),
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
    cpp_args: build_shared ? ['-DVOLT_MIDDLEEND_ANALYSIS_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

volt_middleend_analysis_dep = declare_dependency(
    link_with: volt_middleend_analysis_lib,
    include_directories: volt_middleend_analysis_inc,
    dependencies: [
        volt_middleend_core_dep,
        volt_middleend_ir_dep,
        volt_middleend_typesystem_dep,
        volt_middleend_resolver_dep,
        volt_middleend_consteval_dep,
        volt_middleend_lowering_dep,
        core_dep,
        frontend_dep,
    ],
)
