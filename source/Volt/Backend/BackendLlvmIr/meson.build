backendllvmir_root = meson.current_source_dir()
backendllvmir_found = run_command(
    find_prog,
    backendllvmir_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
backendllvmir_sources = []
foreach p : backendllvmir_found
    backendllvmir_sources += [p.replace(backendllvmir_root + '/', '')]
endforeach

backendllvmir_export = configure_file(
    input: export_template,
    output: 'BackendLlvmIr_export.hpp',
    configuration: {'MODULE': 'BACKENDLLVMIR'},
)

# unity=off, deliberately: this module is ~70 small translation units whose
# whole point is that touching one emitter rebuilds one file. A unity build
# glues them back into the monolith this layout replaced. The PCH (PchLLVM.hpp,
# shared by every LLVM-linking module) is what keeps the non-unity build cheap.
backendllvmir_lib = library(
    'BackendLlvmIr',
    files(backendllvmir_sources),
    backendllvmir_export,
    cpp_pch: llvm_pch_header,
    include_directories: [
        include_directories('Public', 'Private'),
        llvm_include_dirs,
    ],
    dependencies: [compile_options_dep, backendcore_dep, llvm_dep],
    cpp_args: llvm_rtti_args + (
        build_shared ? ['-DBACKENDLLVMIR_BUILD_SHARED'] : []
    ),
    gnu_symbol_visibility: 'hidden',
    install: true,
    override_options: ['unity=off'],
)

backendllvmir_dep = declare_dependency(
    link_with: backendllvmir_lib,
    include_directories: [
        include_directories('Public'),
        include_directories('.'),
    ],
    dependencies: [backendcore_dep, llvm_dep],
)
