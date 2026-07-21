#   volt_per_file_tool(
#     TARGET  <name>       # custom target to create
#     TOOL    <program>    # looked up with find_program
#     SOURCES <files...>   # one tool invocation per file
#     ARGS    <args...>    # inserted before the file
#     DEPENDS <files...>   # extra dependencies (tool config files)
#   )

function( volt_per_file_tool )

  cmake_parse_arguments( T "" "TARGET;TOOL" "SOURCES;ARGS;DEPENDS" ${ARGN} )

  #############################################################################

  string( TOUPPER "${T_TARGET}" TARGET_UPPER )
  find_program( VOLT_TOOL_${TARGET_UPPER} ${T_TOOL} )
  if( NOT VOLT_TOOL_${TARGET_UPPER} )
      message( STATUS "[Volt] Tool: ${T_TOOL} NOT found" )
      return()
  endif()

  if( NOT T_SOURCES )
      message( STATUS "[Volt] No files registered for ${T_TARGET}." )
      return()
  endif()

  list( REMOVE_DUPLICATES T_SOURCES )
  list( SORT T_SOURCES )

  #############################################################################

  set( STAMP_DIR ${CMAKE_BINARY_DIR}/tooling/${T_TARGET} )
  set( STAMPS "" )

  foreach( SOURCE IN LISTS T_SOURCES )
    file( RELATIVE_PATH RELATIVE ${CMAKE_SOURCE_DIR} ${SOURCE} )
    set( STAMP ${STAMP_DIR}/${RELATIVE}.stamp )

    get_filename_component( STAMP_PARENT ${STAMP} DIRECTORY )
    file( MAKE_DIRECTORY ${STAMP_PARENT} )

    add_custom_command(
      OUTPUT  ${STAMP}
      COMMAND ${VOLT_TOOL_${TARGET_UPPER}} ${T_ARGS} ${SOURCE}
      COMMAND ${CMAKE_COMMAND} -E touch ${STAMP}
      DEPENDS ${SOURCE} ${T_DEPENDS}
      COMMENT "[Volt] ${T_TOOL}: ${RELATIVE}"
      VERBATIM
    )
    list( APPEND STAMPS ${STAMP} )
  endforeach()

  add_custom_target( ${T_TARGET} DEPENDS ${STAMPS} )

  #############################################################################

  list( LENGTH T_SOURCES TOTAL )
  message( STATUS
    "[Volt] Tool: ${T_TOOL} enabled (${TOTAL} files, incremental)"
  )

endfunction()
