# Driver::Build() (issue #61) drives whichever concrete backend --target
# selects — today only BackendLLVM ("native") — so it, not the CLI layer, is
# what optionally depends on it. BackendLLVM itself never depends back on
# Driver (Backend* -> Sema only, per rules/ast-value.md's seam), so this
# stays a one-way edge, no cycle.
#
# Driver is added to the build *before* Backend/BackendLLVM (VoltBuild.cmake:
# BackendCore must exist first for Driver's own UnitView mapping), so
# `if( TARGET BackendLLVM )` would not see it yet here — VOLT_ENABLE_LLVM is
# the option() cache variable set before any add_subdirectory runs, and
# BackendLLVM's own CMakeLists find_package(LLVM REQUIRED)s when it is ON, so
# the option being set is exactly as reliable a signal as the target existing.
set( DRIVER_BACKEND_DEPS )
if( VOLT_ENABLE_LLVM )
  list( APPEND DRIVER_BACKEND_DEPS BackendLLVM )
endif()

VoltModule(
  NAME            Driver
  TYPE            shared_library
  SOURCES         "Private/*.cpp"
  PUBLIC_INCLUDES "Public/"
  DEPS            Sema BackendCore ${DRIVER_BACKEND_DEPS}
  PRIVATE_DEFINES VOLT_DEV_STDLIB_DIR="${CMAKE_SOURCE_DIR}/source/Lib"
)

if( VOLT_ENABLE_LLVM )
  target_compile_definitions( Driver PRIVATE VOLT_ENABLE_LLVM )
endif()
