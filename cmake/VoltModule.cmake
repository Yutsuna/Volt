function( VoltModule )

  set(
    MULTI
    SOURCES
    PUBLIC_INCLUDES
    PRIVATE_INCLUDES
    DEFINES
    PRIVATE_DEFINES
    DEPS
  )
  cmake_parse_arguments( M "" "NAME;TYPE" "${MULTI}" ${ARGN} )

  #############################################################################

  set( ALL_SOURCES "" )
  foreach( PATTERN IN LISTS M_SOURCES )
    file(
      GLOB_RECURSE
      MATCHED
      CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${PATTERN}"
    )
    list( APPEND ALL_SOURCES ${MATCHED} )
  endforeach()

  if( NOT ALL_SOURCES )
    return()
  endif()

  file(
    GLOB_RECURSE
    TOOLING_FILES
    CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
  )
  set_property( GLOBAL APPEND PROPERTY VOLT_ALL_FILES
    ${ALL_SOURCES} ${TOOLING_FILES}
  )

  #############################################################################

  string( TOLOWER "${M_TYPE}" TYPE )

  if( TYPE STREQUAL "executable" )
    add_executable( ${M_NAME} ${ALL_SOURCES} )
    add_executable( Volt::${M_NAME} ALIAS ${M_NAME} )
  elseif( TYPE MATCHES "^static" OR NOT VOLT_BUILD_SHARED )
    add_library( ${M_NAME} STATIC ${ALL_SOURCES} )
    add_library( Volt::${M_NAME} ALIAS ${M_NAME} )
  else()
    add_library( ${M_NAME} SHARED ${ALL_SOURCES} )
    set_target_properties( ${M_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON )
    add_library( Volt::${M_NAME} ALIAS ${M_NAME} )
  endif()

  #############################################################################

  target_link_libraries( ${M_NAME} PRIVATE Volt::CompileOptions )
  set_target_properties( ${M_NAME} PROPERTIES
    OUTPUT_NAME "${M_NAME}"
    DEBUG_POSTFIX "_d"
  )

  #############################################################################

  target_include_directories( ${M_NAME} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Public>"
  )
  set_property( GLOBAL APPEND PROPERTY VOLT_PUBLIC_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/Public" )
  foreach( DIR IN LISTS M_PRIVATE_INCLUDES )
    target_include_directories( ${M_NAME} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/${DIR}"
    )
  endforeach()

  #############################################################################

  target_compile_definitions( ${M_NAME} PUBLIC ${M_DEFINES} )
  target_compile_definitions( ${M_NAME} PRIVATE ${M_PRIVATE_DEFINES} )

  #############################################################################

  foreach( DEP IN LISTS M_DEPS )
    if( DEP MATCHES "^Volt::" )
      target_link_libraries( ${M_NAME} PUBLIC ${DEP} )
    else()
      target_link_libraries( ${M_NAME} PUBLIC Volt::${DEP} )
    endif()
  endforeach()

  #############################################################################

  #TODO: add functional testing on
  # Module/
  # |-- Public/
  # |-- Private/
  # |-- Tests/      <<there
  # |-- Benchmarks/ <<there

  #############################################################################

  message( STATUS "[Volt] Module: ${M_NAME}" )

endfunction()
