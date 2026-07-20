function( volt_tidy_lintable_sources SOURCES_VAR REFLECTION_HEADER )
  set( LINTABLE "" )
  set( SKIPPED 0 )

  foreach( SOURCE IN LISTS ${SOURCES_VAR} )
    file( READ "${SOURCE}" CONTENTS )
    if( CONTENTS MATCHES "#include[ \t]*<[ \t]*meta[ \t]*>" OR CONTENTS MATCHES "#include[ \t]*[\"<][^\">]*${REFLECTION_HEADER}" )
      math( EXPR SKIPPED "${SKIPPED} + 1" )
    else()
      list( APPEND LINTABLE ${SOURCE} )
    endif()
  endforeach()

  if( SKIPPED GREATER 0 )
    message( STATUS "[Volt] Tool: clang-tidy skipping ${SKIPPED} reflection TU(s) (P2996 not in LLVM yet)" )
  endif()
  set( ${SOURCES_VAR} ${LINTABLE} PARENT_SCOPE )

endfunction()

#############################################################################

function( volt_enable_tidy )

  get_property( ALL_SOURCES GLOBAL PROPERTY VOLT_ALL_FILES )
  list( FILTER ALL_SOURCES INCLUDE REGEX "\\.cpp$" )
  volt_tidy_lintable_sources( ALL_SOURCES "Meta/Reflect\\.hpp" )

  #############################################################################

  set( TIDY_DB_DIR ${CMAKE_BINARY_DIR}/tooling )
  set( TIDY_DB     ${TIDY_DB_DIR}/compile_commands.json )
  file( MAKE_DIRECTORY ${TIDY_DB_DIR} )

  add_custom_command(
    OUTPUT  ${TIDY_DB}
    COMMAND sh -c "sed -E 's/ -freflection//g; s/ -Winvalid-pch//g; s/ -include [^ ]*cmake_pch\\.hxx//g' '${CMAKE_BINARY_DIR}/compile_commands.json' > '${TIDY_DB}.tmp'"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TIDY_DB}.tmp ${TIDY_DB}
    DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
    COMMENT "[Volt] clang-tidy: refreshing stripped compile database"
    VERBATIM
  )

  #############################################################################

  volt_per_file_tool(
    TARGET  tidy
    TOOL    clang-tidy
    SOURCES ${ALL_SOURCES}
    ARGS    --quiet -p ${TIDY_DB_DIR}
    DEPENDS ${CMAKE_SOURCE_DIR}/.clang-tidy ${TIDY_DB}
  )

  #############################################################################

endfunction()
