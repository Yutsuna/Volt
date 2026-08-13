volt_middleend_lowering_inc = [
    include_directories('Public'),
    include_directories('.'),
]

volt_middleend_lowering_export = configure_file(
    input: export_template,
    output: 'VoltMiddleEndLowering_export.hpp',
    configuration: {'MODULE': 'VOLT_MIDDLEEND_LOWERING'},
)

volt_middleend_lowering_lib = static_library(
    'VoltMiddleEndLowering',
    files(
        'Private/LoweringRules.cpp',
        'Private/FunctionalLowering.cpp',
        'Private/PipelineLowering.cpp',
        'Private/JsxLowering.cpp',
        'Private/CaseLowering.cpp',
        'Private/DotCallLowering.cpp',
        'Private/AssignLowering.cpp',
        'Private/IndexLowering.cpp',
        'Private/InterpLowering.cpp',
        'Private/LiteralLowering.cpp',
        'Private/EnumCaseLowering.cpp',
        'Private/ClosureLifting.cpp',
        'Private/FinalizeLowering.cpp',
    ),
    volt_middleend_lowering_export,
    cpp_pch: common_pch_header,
    include_directories: [
        volt_middleend_lowering_inc,
        # The back-edge against §1's DAG, header-only and deliberate: the four
        # post-TypeChecker sweeps (LiteralLowering, EnumCaseLowering,
        # ClosureLifting, FinalizeLowering) rewrite *within* Analysis's walk
        # state, so they take an `Analysis::TypeCheckerContext &` and reach
        # its inferencers (ExprInferencer, MemberResolver, DeclStmtWalker).
        # Nothing in Analysis's headers points back here, so the #include
        # graph itself stays acyclic; only the link is mutual, which is what
        # `link_whole` at the MiddleEnd top level answers.
        include_directories('../Analysis/Public'),
        include_directories('../Analysis/Private'),
        include_directories('../Analysis'),
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
    cpp_args: build_shared ? ['-DVOLT_MIDDLEEND_LOWERING_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

volt_middleend_lowering_dep = declare_dependency(
    link_with: volt_middleend_lowering_lib,
    include_directories: volt_middleend_lowering_inc,
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
