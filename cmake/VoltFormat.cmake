function( volt_enable_formatting )

  #############################################################################

  find_program( CLANG_FORMAT clang-format )
  if( NOT CLANG_FORMAT )
      message( STATUS "[Volt] Tool: clang-format NOT found" )
      return()
  endif()

  #############################################################################

  get_property( ALL_SOURCES GLOBAL PROPERTY VOLT_ALL_FILES )
  if( NOT ALL_SOURCES )
      message( STATUS "[Volt] No files registered for formatting." )
      return()
  endif()

  list( REMOVE_DUPLICATES ALL_SOURCES )

  #############################################################################

  add_custom_target( format
    COMMAND ${CLANG_FORMAT} -i -style=file ${ALL_SOURCES}
    COMMENT "[Volt] Formatting source files with clang-format"
    VERBATIM
  )

  message( STATUS "[Volt] Tool: clang-format enabled" )

  #############################################################################

endfunction()
