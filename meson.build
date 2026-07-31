#{
# meson.build
# Meson entry-point.
#}

project('volt', 'cpp', 'c',
  version : run_command('cat', 'VERSION.md', check : true).stdout().strip(),
  license : 'MIT',
  default_options : [
    'cpp_std=c++26',
    'c_std=c23',
    'default_library=shared',
    'warning_level=0',
    'unity=on',
    'unity_size=4',
  ],
  meson_version : '>=1.10',
)

subdir('meson')
subdir('source/Volt')
