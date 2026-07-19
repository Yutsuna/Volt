function( VoltAddModules BASE_DIR )
  foreach( MODULE IN LISTS ARGN )
    add_subdirectory( ${BASE_DIR}/${MODULE} )
  endforeach()
endfunction()


VoltAddModules(
  ${CMAKE_CURRENT_SOURCE_DIR}/source/Volt/
  Core
  Frontend
  Sema
  Driver
  Volt
)


volt_enable_formatting()
