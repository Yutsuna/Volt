# LlvmRun.cmake — the acceptance test: compile a sample all the way to a
# linked native executable, run it, and compare what it produced against the
# `.expected` file beside it.
#
# This is the only test that exercises the ABI end to end. Every other suite
# stops at the middle-end: Golden compares an AST dump, Check runs the passes,
# AstInvariant replays the two AST contracts. Only running the binary proves
# that field offsets, the parameter order, the aggregate convention and the
# instruction selection all agree with each other.
#
# `.expected` is one `exit=<code>` line, optionally followed by the exact
# stdout the program must produce. Today every sample is exit-code only: the
# stdlib declares no output facility yet (rules/core-ast.md's known gaps), so
# the exit code is the whole observable result. The stdout half is here so
# that adding one is a data change and not a test-harness change.
#
# Inputs: VOLT_BIN, SOURCE (relative to the repo root), WORK (a scratch dir).

get_filename_component( ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE )

if( NOT DEFINED WORK )
  set( WORK ${CMAKE_CURRENT_BINARY_DIR} )
endif()

get_filename_component( STEM ${SOURCE} NAME_WE )
set( ARTIFACT ${WORK}/llvmrun-${STEM} )
file( MAKE_DIRECTORY ${WORK} )

#############################################################################

execute_process(
  COMMAND           ${VOLT_BIN} build ${SOURCE} -o ${ARTIFACT}
  WORKING_DIRECTORY ${ROOT}
  RESULT_VARIABLE   BUILD_STATUS
  OUTPUT_VARIABLE   BUILD_OUTPUT
  ERROR_VARIABLE    BUILD_ERRORS
)

if( NOT BUILD_STATUS EQUAL 0 )
  message( FATAL_ERROR
    "LlvmRun: `volt build ${SOURCE}` failed (${BUILD_STATUS}):\n${BUILD_OUTPUT}${BUILD_ERRORS}" )
endif()

#############################################################################

execute_process(
  COMMAND           ${ARTIFACT}
  WORKING_DIRECTORY ${ROOT}
  RESULT_VARIABLE   RUN_STATUS
  OUTPUT_VARIABLE   RUN_OUTPUT
  ERROR_VARIABLE    RUN_ERRORS
)

# A signal is reported as a string ("Segmentation fault"), never a number, so
# comparing against the expected code would silently pass it through.
if( NOT RUN_STATUS MATCHES "^[0-9]+$" )
  message( FATAL_ERROR "LlvmRun: ${SOURCE} did not exit normally: ${RUN_STATUS}\n${RUN_ERRORS}" )
endif()

#############################################################################

file( READ ${ROOT}/${SOURCE}.expected EXPECTED )
string( REGEX MATCH "exit=([0-9]+)" EXIT_MATCH "${EXPECTED}" )
if( NOT EXIT_MATCH )
  message( FATAL_ERROR "LlvmRun: ${SOURCE}.expected has no `exit=<code>` line" )
endif()
set( EXPECTED_EXIT ${CMAKE_MATCH_1} )

string( REGEX REPLACE "^[^\n]*\n?" "" EXPECTED_OUTPUT "${EXPECTED}" )

if( NOT RUN_STATUS EQUAL EXPECTED_EXIT )
  message( FATAL_ERROR
    "LlvmRun: ${SOURCE} exited ${RUN_STATUS}, expected ${EXPECTED_EXIT}\n${RUN_OUTPUT}${RUN_ERRORS}" )
endif()

if( NOT "${RUN_OUTPUT}" STREQUAL "${EXPECTED_OUTPUT}" )
  message( FATAL_ERROR
    "LlvmRun: ${SOURCE} stdout mismatch.\n--- expected ---\n${EXPECTED_OUTPUT}\n--- actual ---\n${RUN_OUTPUT}" )
endif()

file( REMOVE ${ARTIFACT} )
