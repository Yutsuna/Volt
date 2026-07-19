function( volt_enable_tidy )

  #############################################################################

  get_property( ALL_SOURCES GLOBAL PROPERTY VOLT_ALL_FILES )
  list( FILTER ALL_SOURCES INCLUDE REGEX "\\.cpp$" )

  volt_per_file_tool(
    TARGET  tidy
    TOOL    clang-tidy
    SOURCES ${ALL_SOURCES}
    ARGS    --quiet -p ${CMAKE_BINARY_DIR}
    DEPENDS ${CMAKE_SOURCE_DIR}/.clang-tidy
  )

  #############################################################################

endfunction()
