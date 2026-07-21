VoltModule(
  NAME            Driver
  TYPE            shared_library
  SOURCES         "Private/*.cpp"
  PUBLIC_INCLUDES "Public/"
  DEPS            Sema
)
