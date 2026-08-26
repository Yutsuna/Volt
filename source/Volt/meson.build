#{
# Volt/meson.build
# Declare the project's submodules here.
#}

subdir('Core')
subdir('Frontend')
subdir('MiddleEnd')
subdir('Backend/BackendCore')
# Order is the dependency order: BackendLlvmIr owns the AST -> llvm::Module
# layer, BackendLLVM is the ahead-of-time tail over it.
if get_option('enable_llvm')
    subdir('Backend/BackendLlvmIr')
    subdir('Backend/BackendLLVM')
    if jit_enabled
        subdir('Backend/BackendJIT')
    endif
endif
subdir('Driver')
subdir('Backend/BackendWASM')
subdir('REPL')
subdir('Volt')
