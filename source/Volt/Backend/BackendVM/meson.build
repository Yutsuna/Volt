backendvm_root = meson.current_source_dir()
backendvm_found = run_command(find_prog, backendvm_root / 'Private', '-name', '*.cpp', check : true).stdout().strip().split('\n')
backendvm_sources = []
foreach p : backendvm_found
  backendvm_sources += [p.replace(backendvm_root + '/', '')]
endforeach

backendvm_export = configure_file(input : export_template, output : 'BackendVM_export.hpp', configuration : {'MODULE' : 'BACKENDVM'})

backendvm_lib = library('BackendVM', files(backendvm_sources), backendvm_export,
  cpp_pch : common_pch_header,
  include_directories : include_directories('Public', 'Private'),
  dependencies : [compile_options_dep, backendcore_dep],
  cpp_args : build_shared ? ['-DBACKENDVM_BUILD_SHARED'] : [],
  gnu_symbol_visibility : 'hidden',
  install : true,
  override_options : ['unity=off'],
)

backendvm_dep = declare_dependency(
  link_with : backendvm_lib,
  include_directories : [include_directories('Public'), include_directories('.')],
  dependencies : backendcore_dep,
)
