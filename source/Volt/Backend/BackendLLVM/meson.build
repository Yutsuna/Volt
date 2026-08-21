backendllvm_root = meson.current_source_dir()
backendllvm_found = run_command(
    find_prog,
    backendllvm_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
backendllvm_sources = []
foreach p : backendllvm_found
    backendllvm_sources += [p.replace(backendllvm_root + '/', '')]
endforeach

backendllvm_export = configure_file(
    input: export_template,
    output: 'BackendLLVM_export.hpp',
    configuration: {'MODULE': 'BACKENDLLVM'},
)

# unity=off, for the same reason BackendLlvmIr sets it: one edit, one TU. What
# is left here is the AOT tail — verify, optimise, `.o`, link — over a module
# BackendLlvmIr produced.
backendllvm_lib = library(
    'BackendLLVM',
    files(backendllvm_sources),
    backendllvm_export,
    cpp_pch: llvm_pch_header,
    include_directories: [
        include_directories('Public', 'Private'),
        llvm_include_dirs,
    ],
    dependencies: [compile_options_dep, backendllvmir_dep, llvm_dep],
    cpp_args: llvm_rtti_args + (
        build_shared ? ['-DBACKENDLLVM_BUILD_SHARED'] : []
    ),
    gnu_symbol_visibility: 'hidden',
    install: true,
    override_options: ['unity=off'],
)

backendllvm_dep = declare_dependency(
    link_with: backendllvm_lib,
    include_directories: [
        include_directories('Public'),
        include_directories('.'),
    ],
    dependencies: backendllvmir_dep,
)
