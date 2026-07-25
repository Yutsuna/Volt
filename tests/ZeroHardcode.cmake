get_filename_component( ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE )

set( TYPE_NAMES "String|Array|Int|Int32|Int64|UInt8|UInt32|UInt64|Float32|Float64|Bool|Char|Nil|Hash" )
set( TYPE_PATTERN "(^|[^A-Za-z0-9_])(${TYPE_NAMES})($|[^A-Za-z0-9_])" )
set( SYMBOL_PATTERN "(^|[^A-Za-z0-9_])Symbol($|[^A-Za-z0-9_])" )
set( COMMENT_PATTERN "^[ \t]*(//|/\\*|\\*)" )

#############################################################################

file( GLOB_RECURSE SCANNED_FILES
  ${ROOT}/source/Volt/Frontend/*.hpp
  ${ROOT}/source/Volt/Frontend/*.cpp
  ${ROOT}/source/Volt/Frontend/*.inl
  ${ROOT}/source/Volt/Sema/*.hpp
  ${ROOT}/source/Volt/Sema/*.cpp
  ${ROOT}/source/Volt/Sema/*.inl
  ${ROOT}/source/Volt/Backend/*.hpp
  ${ROOT}/source/Volt/Backend/*.cpp
  ${ROOT}/source/Volt/Backend/*.inl )
list( SORT SCANNED_FILES )

set( VIOLATIONS "" )
foreach( FILE_PATH IN LISTS SCANNED_FILES )

  #############################################################################

  if( FILE_PATH MATCHES "SelfCheck\\.cpp$" )
    continue()
  endif()
  file( READ ${FILE_PATH} CONTENT )

  #############################################################################

  string( REPLACE ";" "\\;" CONTENT "${CONTENT}" )
  string( REPLACE "\n" ";L:" LINES "L:${CONTENT}" )
  set( LINE_NUMBER 0 )

  #############################################################################

  foreach( LINE IN LISTS LINES )
    math( EXPR LINE_NUMBER "${LINE_NUMBER} + 1" )
    string( SUBSTRING "${LINE}" 2 -1 LINE )
    if( LINE MATCHES "${COMMENT_PATTERN}" OR LINE MATCHES "${SYMBOL_PATTERN}" )
      continue()
    endif()
    if( LINE MATCHES "${TYPE_PATTERN}" )
      file( RELATIVE_PATH RELATIVE_FILE ${ROOT} ${FILE_PATH} )
      string( APPEND VIOLATIONS "  ${RELATIVE_FILE}:${LINE_NUMBER}: ${LINE}\n" )
    endif()
  endforeach()

  #############################################################################

endforeach()

#############################################################################

if( VIOLATIONS )
  message( FATAL_ERROR
    "ZeroHardcode: Volt type names leaked into the C++ compiler:\n${VIOLATIONS}"
  )
endif()
message( STATUS "ZeroHardcode: no hardcoded Volt type names in Frontend/Sema/Backend" )
