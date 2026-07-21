#############################################################################

function( volt_enable_formatting )

  #############################################################################

  get_property( ALL_SOURCES GLOBAL PROPERTY VOLT_ALL_FILES )

  volt_per_file_tool(
    TARGET  format
    TOOL    clang-format
    SOURCES ${ALL_SOURCES}
    ARGS    -i -style=file
    DEPENDS ${CMAKE_SOURCE_DIR}/.clang-format
  )

  #############################################################################

endfunction()
