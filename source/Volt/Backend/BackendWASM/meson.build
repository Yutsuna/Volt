backendwasm_root = meson.current_source_dir()
backendwasm_found = run_command(find_prog, backendwasm_root / 'Private', '-name', '*.cpp', check : true).stdout().strip().split('\n')
backendwasm_sources = []
foreach p : backendwasm_found
  backendwasm_sources += [p.replace(backendwasm_root + '/', '')]
endforeach

backendwasm_export = configure_file(input : export_template, output : 'BackendWASM_export.hpp', configuration : {'MODULE' : 'BACKENDWASM'})

backendwasm_lib = library('BackendWASM', files(backendwasm_sources), backendwasm_export,
  cpp_pch : common_pch_header,
  include_directories : include_directories('Public', 'Private'),
  dependencies : [compile_options_dep, backendcore_dep],
  cpp_args : build_shared ? ['-DBACKENDWASM_BUILD_SHARED'] : [],
  gnu_symbol_visibility : 'hidden',
  install : true,
  override_options : ['unity=off'],
)

backendwasm_dep = declare_dependency(
  link_with : backendwasm_lib,
  include_directories : [include_directories('Public'), include_directories('.')],
  dependencies : backendcore_dep,
)
