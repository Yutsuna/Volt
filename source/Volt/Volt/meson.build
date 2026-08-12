volt_root = meson.current_source_dir()
volt_found = run_command(
    find_prog,
    volt_root / 'Private',
    '-name',
    '*.cpp',
    check: true,
).stdout().strip().split(
    '\n',
)
volt_sources = []
foreach p : volt_found
    volt_sources += [p.replace(volt_root + '/', '')]
endforeach

volt_exe = executable(
    'volt',
    files(volt_sources),
    cpp_pch: common_pch_header,
    include_directories: include_directories('Public'),
    dependencies: [compile_options_dep, driver_dep],
    gnu_symbol_visibility: 'hidden',
    install: true,
)
