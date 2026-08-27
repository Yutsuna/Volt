replsyntax_root = meson.current_source_dir()
replsyntax_found = run_command(
    find_prog,
    replsyntax_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
replsyntax_sources = []
foreach p : replsyntax_found
    replsyntax_sources += [p.replace(replsyntax_root + '/', '')]
endforeach

replsyntax_export = configure_file(
    input: export_template,
    output: 'ReplSyntax_export.hpp',
    configuration: {'MODULE': 'REPLSYNTAX'},
)

# Frontend, for the lexer: Volt is coloured by the compiler's own tokenizer and
# never by a second grammar kept here. The IR and machine-code scanners bring
# no dependency at all — that is why they are hand-written rather than LLVM's.
replsyntax_lib = library(
    'ReplSyntax',
    files(replsyntax_sources),
    replsyntax_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public', 'Private'),
    dependencies: [compile_options_dep, repldoc_dep, frontend_dep],
    cpp_args: build_shared ? ['-DREPLSYNTAX_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

replsyntax_dep = declare_dependency(
    link_with: replsyntax_lib,
    include_directories: [include_directories('Public'), include_directories('.')],
    dependencies: [repldoc_dep, frontend_dep],
)
