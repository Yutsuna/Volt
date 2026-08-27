replcomplete_root = meson.current_source_dir()
replcomplete_found = run_command(
    find_prog,
    replcomplete_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
replcomplete_sources = []
foreach p : replcomplete_found
    replcomplete_sources += [p.replace(replcomplete_root + '/', '')]
endforeach

replcomplete_export = configure_file(
    input: export_template,
    output: 'ReplComplete_export.hpp',
    configuration: {'MODULE': 'REPLCOMPLETE'},
)

# ReplQuery is here for one thing only: the builtin table, so that `:d` + Tab
# offers `:doc` without a second copy of the list.
replcomplete_lib = library(
    'ReplComplete',
    files(replcomplete_sources),
    replcomplete_export,
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: [compile_options_dep, repldoc_dep, repleval_dep, replquery_dep],
    cpp_args: build_shared ? ['-DREPLCOMPLETE_BUILD_SHARED'] : [],
    gnu_symbol_visibility: 'hidden',
    install: true,
)

replcomplete_dep = declare_dependency(
    link_with: replcomplete_lib,
    include_directories: [include_directories('Public'), include_directories('.')],
    dependencies: [repldoc_dep, repleval_dep, replquery_dep],
)
