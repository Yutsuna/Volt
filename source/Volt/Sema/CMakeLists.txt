VoltModule(
  NAME            Sema
  TYPE            shared_library
  SOURCES         "Private/*.cpp"
  PUBLIC_INCLUDES "Public/"
  PRIVATE_INCLUDES "Private/"
  DEPS            Frontend
)
