function( VoltAddModules BASE_DIR )

  foreach( MODULE IN LISTS ARGN )
    add_subdirectory( ${BASE_DIR}/${MODULE} )
  endforeach()

endfunction()

#############################################################################

VoltAddModules(
  ${CMAKE_CURRENT_SOURCE_DIR}/source/Volt/
  Core
  Frontend
  Sema
  # Before Driver: the Driver maps its CompileUnits into Backend::UnitViews, so
  # `Volt::BackendCore` must already be a defined ALIAS when Driver links it.
  # The dependency stays one-way — BackendCore never mentions Driver.
  Backend/BackendCore
  Driver
  Backend/BackendVM
  Backend/BackendLLVM
  Backend/BackendWASM
  Volt
)

#############################################################################

volt_enable_formatting()
volt_enable_tidy()
