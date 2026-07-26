VoltModule(
  NAME            Driver
  TYPE            shared_library
  SOURCES         "Private/*.cpp"
  PUBLIC_INCLUDES "Public/"
  DEPS            Sema BackendCore
  PRIVATE_DEFINES VOLT_DEV_STDLIB_DIR="${CMAKE_SOURCE_DIR}/source/Lib"
)
