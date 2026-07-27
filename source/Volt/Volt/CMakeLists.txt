VoltModule(
  NAME            Volt
  TYPE            executable
  SOURCES         "Private/*.cpp"
  PUBLIC_INCLUDES "Public/"
  DEPS            Driver
)

set_target_properties( Volt PROPERTIES OUTPUT_NAME "volt" )
