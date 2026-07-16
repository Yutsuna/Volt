VoltModule(
  NAME            Frontend
  TYPE            shared_library
  SOURCES         "Private/*.cpp"
  PUBLIC_INCLUDES "Public/"
  DEPS            Core
)
