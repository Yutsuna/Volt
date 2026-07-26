set( VOLT_EXE_DEPS Driver )
if( TARGET BackendLLVM )
  list( APPEND VOLT_EXE_DEPS BackendLLVM )
endif()

VoltModule(
  NAME            Volt
  TYPE            executable
  SOURCES         "Private/*.cpp"
  PUBLIC_INCLUDES "Public/"
  DEPS            ${VOLT_EXE_DEPS}
)

if( TARGET BackendLLVM )
  target_compile_definitions( Volt PRIVATE VOLT_ENABLE_LLVM )
endif()
