subdir('Core')
subdir('IR')
subdir('TypeSystem')
subdir('Resolver')
subdir('ConstEval')
subdir('Lowering')
subdir('Analysis')
subdir('Optimisations')

# `link_whole`, not `link_with`, and the reason is the one back-edge §1's DAG
# does not have: `TypeSystem/Private/Reinstantiate.cpp` re-walks a generic
# body per instantiation, which means driving `Analysis::TypeCheckerContext`
# and `Lowering::LowerClosureLit`; and `Analysis::TypeChecker` in turn calls
# the `Lowering` sweeps that take its own context. Static archives resolve
# left-to-right, so a genuine cycle among three of them cannot be satisfied by
# any single ordering. Pulling every object in unconditionally sidesteps
# archive ordering entirely — and costs nothing here, since `PassRegistry`
# already names every pass in the manifest and therefore reaches every
# translation unit anyway.
volt_middleend_dep = declare_dependency(
    link_whole: [
        volt_middleend_core_lib,
        volt_middleend_ir_lib,
        volt_middleend_typesystem_lib,
        volt_middleend_resolver_lib,
        volt_middleend_consteval_lib,
        volt_middleend_lowering_lib,
        volt_middleend_analysis_lib,
        volt_middleend_optimisations_lib,
    ],
    include_directories: [
        volt_middleend_core_inc,
        volt_middleend_ir_inc,
        volt_middleend_typesystem_inc,
        volt_middleend_resolver_inc,
        volt_middleend_consteval_inc,
        volt_middleend_lowering_inc,
        volt_middleend_analysis_inc,
        volt_middleend_optimisations_inc,
    ],
    dependencies: [
        core_dep,
        frontend_dep,
    ],
)

alias_target(
    'MiddleEnd',
    [
        volt_middleend_core_lib,
        volt_middleend_ir_lib,
        volt_middleend_typesystem_lib,
        volt_middleend_resolver_lib,
        volt_middleend_consteval_lib,
        volt_middleend_lowering_lib,
        volt_middleend_analysis_lib,
        volt_middleend_optimisations_lib,
    ],
)

