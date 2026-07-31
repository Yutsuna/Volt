llvm_dep = dependency('llvm', method : 'config-tool', static : false, required : true)

llvm_include_dirs = include_directories(
  llvm_dep.get_variable(cmake : 'LLVM_INCLUDE_DIRS', default_value : '').split(';'),
  is_system : true,
)
llvm_rtti = llvm_dep.get_variable(cmake : 'LLVM_ENABLE_RTTI', default_value : 'ON')
backendllvm_rtti_args = llvm_rtti in ['OFF', '0', 'NO'] ? ['-fno-rtti'] : []

backendllvm_root = meson.current_source_dir()
backendllvm_found = run_command(find_prog, backendllvm_root / 'Private', '-name', '*.cpp', check : true).stdout().strip().split('\n')
backendllvm_sources = []
foreach p : backendllvm_found
  backendllvm_sources += [p.replace(backendllvm_root + '/', '')]
endforeach

backendllvm_export = configure_file(input : export_template, output : 'BackendLLVM_export.hpp', configuration : {'MODULE' : 'BACKENDLLVM'})

backendllvm_lib = library('BackendLLVM', files(backendllvm_sources), backendllvm_export,
  cpp_pch : llvm_pch_header,
  include_directories : [include_directories('Public', 'Private'), llvm_include_dirs],
  dependencies : [compile_options_dep, backendcore_dep, llvm_dep],
  cpp_args : backendllvm_rtti_args + (build_shared ? ['-DBACKENDLLVM_BUILD_SHARED'] : []),
  gnu_symbol_visibility : 'hidden',
  install : true,
  override_options : ['unity_size=8'],
)

backendllvm_dep = declare_dependency(
  link_with : backendllvm_lib,
  include_directories : [include_directories('Public'), include_directories('.')],
  dependencies : backendcore_dep,
)
