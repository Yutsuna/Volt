#{
# meson/meson.build
# Declare flags & options
#}

cxx = meson.get_compiler('cpp')
is_gnu = cxx.get_id() == 'gcc'
buildtype = get_option('buildtype')
is_debuglike = buildtype in ['debug', 'debugoptimized']
build_shared = get_option('default_library') == 'shared'
debug_full = get_option('debug_full')

find_prog = find_program('find')

# -----------------------------------------------------------------------------

reflection_flag = {'gcc' : ['-freflection']}
uninit_warn_flag = {'gcc' : ['-Wno-maybe-uninitialized']}

common_cpp_args = reflection_flag.get(cxx.get_id(), [])
common_cpp_args += [
  '-Wall', '-Wextra', '-Wpedantic', '-Wshadow', '-Wnon-virtual-dtor',
  '-Wold-style-cast', '-Wcast-align', '-Wunused', '-Woverloaded-virtual',
  '-Wconversion', '-Wsign-conversion', '-Wnull-dereference',
  '-Wdouble-promotion', '-Wformat=2', '-Wimplicit-fallthrough',
  '-Wno-missing-braces', '-Werror',
  '-fvisibility=hidden', '-fvisibility-inlines-hidden',
]
common_cpp_args += cxx.get_supported_arguments(
  uninit_warn_flag.get(cxx.get_id(), ['-Wno-uninitialized']),
)

common_link_args = []

debug_gflag = debug_full ? '-g3' : '-g1'
debug_opt_flags = ['-Og', debug_gflag, '-fno-omit-frame-pointer']
common_cpp_args += buildtype == 'debug' ? debug_opt_flags : []

common_cpp_args += (is_gnu and is_debuglike) ? ['-gsplit-dwarf', '-fdebug-types-section'] : []
common_link_args += (is_gnu and is_debuglike) ? ['-Wl,--gdb-index'] : []

common_cpp_args += get_option('checked_ids') ? ['-DVOLT_CHECKED_IDS=1'] : []

# ---------------------------------------------------------------------------

sanitizers = {
  'enable_asan' : 'address',
  'enable_ubsan' : 'undefined',
  'enable_tsan' : 'thread',
}
foreach opt, name : sanitizers
  active = get_option(opt) and is_debuglike
  common_cpp_args += active ? ['-fsanitize=@0@'.format(name)] : []
  common_link_args += active ? ['-fsanitize=@0@'.format(name)] : []
  common_cpp_args += (active and name != 'undefined') ? ['-fno-omit-frame-pointer'] : []
endforeach

mold = find_program('mold', required : false)
common_link_args += mold.found() ? ['-fuse-ld=mold', '-Wl,--icf=all'] : []

if get_option('enable_lto') and buildtype == 'release'
  add_project_arguments('-flto=auto', language : 'cpp')
  add_project_link_arguments('-flto=auto', language : 'cpp')
endif

# ---------------------------------------------------------------------------

volt_inc = include_directories('../source/Volt')

compile_options_dep = declare_dependency(
  compile_args : common_cpp_args,
  link_args : common_link_args,
  include_directories : volt_inc,
)

export_template = files('export_template.hpp.inl')

add_project_arguments('-MMD', language : 'cpp')
common_pch_header = meson.project_source_root() / 'source' / 'Volt' / 'Pch.hpp'
llvm_pch_header = meson.project_source_root() / 'source' / 'Volt' / 'Backend' / 'BackendLLVM' / 'PchLLVM.hpp'
