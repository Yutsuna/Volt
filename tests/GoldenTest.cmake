#############################################################################

get_filename_component( ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE )
set( GOLDEN_DIR ${ROOT}/tests/golden )

#############################################################################

if( NOT DEFINED VOLT_BIN )
  foreach( CANDIDATE ${ROOT}/build/bin/Volt_d ${ROOT}/build/bin/volt )
    if( EXISTS ${CANDIDATE} )
      set( VOLT_BIN ${CANDIDATE} )
      break()
    endif()
  endforeach()
endif()
if( NOT DEFINED VOLT_BIN OR NOT EXISTS ${VOLT_BIN} )
  message( FATAL_ERROR
    "GoldenTest: compiler not found (build first, or pass -DVOLT_BIN=)"
  )
endif()

#############################################################################

function( VoltParse SOURCE LOWERED OUT_VAR )

  set( PARSE_ARGS parse --no-color -i ${SOURCE} )
  if( LOWERED )
    list( APPEND PARSE_ARGS --lowered )
  endif()

  #############################################################################

  execute_process(
    COMMAND ${VOLT_BIN} ${PARSE_ARGS}
    WORKING_DIRECTORY ${ROOT}
    OUTPUT_VARIABLE STD_OUT
    ERROR_VARIABLE STD_ERR
    RESULT_VARIABLE EXIT_CODE
  )
  #############################################################################

  set( ${OUT_VAR} "${STD_OUT}${STD_ERR}--- exit ${EXIT_CODE} ---\n" PARENT_SCOPE )
endfunction()

#############################################################################

function( VoltGoldenPath SOURCE LOWERED OUT_VAR )
  if( LOWERED )
    set( ${OUT_VAR} ${GOLDEN_DIR}/${SOURCE}.lowered.golden PARENT_SCOPE )
  else()
    set( ${OUT_VAR} ${GOLDEN_DIR}/${SOURCE}.golden PARENT_SCOPE )
  endif()
endfunction()

#############################################################################

if( UPDATE )
  file( GLOB_RECURSE SAMPLES RELATIVE
    ${ROOT} ${ROOT}/samples/*.vl ${ROOT}/samples/*.vlx
  )
  list( SORT SAMPLES )

  #############################################################################

  foreach( SOURCE IN LISTS SAMPLES )
    foreach( LOWERED 0 1 )
      VoltParse( ${SOURCE} ${LOWERED} ACTUAL )
      VoltGoldenPath( ${SOURCE} ${LOWERED} GOLDEN )
      file( WRITE ${GOLDEN} "${ACTUAL}" )
    endforeach()
    message( STATUS "UPDATED ${SOURCE} (+ lowered)" )
  endforeach()

  #############################################################################

  list( LENGTH SAMPLES TOTAL )
  message( STATUS
    "GoldenTest: 2x${TOTAL} golden(s) written under tests/golden/"
  )
  return()
endif()

#############################################################################

if( NOT DEFINED SOURCE )
  message( FATAL_ERROR "GoldenTest: pass -DSOURCE=samples/... or -DUPDATE=1" )
endif()
if( NOT DEFINED LOWERED )
  set( LOWERED 0 )
endif()

#############################################################################

VoltGoldenPath( ${SOURCE} ${LOWERED} GOLDEN )
if( NOT EXISTS ${GOLDEN} )
  message( FATAL_ERROR
    "GoldenTest: missing ${GOLDEN} (run the golden-update target)"
  )
endif()

#############################################################################

VoltParse( ${SOURCE} ${LOWERED} ACTUAL )
file( READ ${GOLDEN} EXPECTED )
if( ACTUAL STREQUAL EXPECTED )
  return()
endif()

#############################################################################

if( LOWERED )
  set( ACTUAL_FILE ${ROOT}/build/golden-actual/${SOURCE}.lowered.actual )
else()
  set( ACTUAL_FILE ${ROOT}/build/golden-actual/${SOURCE}.actual )
endif()

#############################################################################

file( WRITE ${ACTUAL_FILE} "${ACTUAL}" )
find_program( DIFF_TOOL diff )
set( DIFF_TEXT "" )
if( DIFF_TOOL )
  execute_process( COMMAND
    ${DIFF_TOOL} -u ${GOLDEN} ${ACTUAL_FILE} OUTPUT_VARIABLE DIFF_TEXT
  )
endif()
message( FATAL_ERROR
  "GoldenTest: ${SOURCE} diverges from tests/golden/ \
  (run the golden-update target if intended)\n${DIFF_TEXT}"
)
