#{
# Volt/meson.build
# Declare the project's submodules here.
#}

subdir('Core')
subdir('Frontend')
subdir('Sema')
subdir('Backend/BackendCore')
if get_option('enable_llvm')
  subdir('Backend/BackendLLVM')
endif
subdir('Driver')
subdir('Backend/BackendVM')
subdir('Backend/BackendWASM')
subdir('Volt')
