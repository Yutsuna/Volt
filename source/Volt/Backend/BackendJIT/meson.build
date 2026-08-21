backendjit_root = meson.current_source_dir()
backendjit_found = run_command(
    find_prog,
    backendjit_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
backendjit_sources = []
foreach p : backendjit_found
    backendjit_sources += [p.replace(backendjit_root + '/', '')]
endforeach

backendjit_export = configure_file(
    input: export_template,
    output: 'BackendJIT_export.hpp',
    configuration: {'MODULE': 'BACKENDJIT'},
)

# The shared PchLLVM.hpp, not one of its own. A second PCH is what the plan
# sketched — keeping ORC's headers out of the other two modules' PCH — but GCC
# maps a PCH at a fixed address and refuses a second one here ("PCH allocation
# failure"). It costs nothing to give up: this module is two translation units,
# so the ORC headers are parsed twice rather than once, and the shared PCH still
# covers everything under llvm/IR.
backendjit_lib = library(
    'BackendJIT',
    files(backendjit_sources),
    backendjit_export,
    cpp_pch: llvm_pch_header,
    include_directories: [
        include_directories('Public', 'Private'),
        llvm_include_dirs,
    ],
    dependencies: [compile_options_dep, backendllvmir_dep, llvm_dep],
    cpp_args: llvm_rtti_args + (
        build_shared ? ['-DBACKENDJIT_BUILD_SHARED'] : []
    ),
    gnu_symbol_visibility: 'hidden',
    install: true,
    override_options: ['unity=off'],
)

backendjit_dep = declare_dependency(
    link_with: backendjit_lib,
    include_directories: [
        include_directories('Public'),
        include_directories('.'),
    ],
    dependencies: [backendllvmir_dep, llvm_dep],
)
