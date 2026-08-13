subdir('Core')
subdir('IR')
subdir('TypeSystem')
subdir('Resolver')
subdir('ConstEval')
subdir('Lowering')
subdir('Analysis')

volt_middleend_dep = declare_dependency(
  dependencies : [
    volt_middleend_core_dep,
    volt_middleend_ir_dep,
    volt_middleend_typesystem_dep,
    volt_middleend_resolver_dep,
    volt_middleend_consteval_dep,
  ],
)
