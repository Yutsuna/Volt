# LlvmIr.cmake — `volt build --emit ir` on a sample, checked for the two
# things about the IR that are actually contracts.
#
# Deliberately *not* a golden comparison of the text. The module carries the
# whole stdlib, so a golden would be several thousand lines that any change to
# `source/Lib` rewrites wholesale, and it would fail on value numbering long
# before it ever caught a codegen bug. What matters, and what is checked here:
#
#   - the module is emitted at all, and `llvm::verifyModule` accepted it
#     (Finalize runs the verifier before writing anything, so a zero exit is
#     already that guarantee);
#   - the C entry point exists under its unmangled name, which is what makes
#     the artifact a program rather than a pile of symbols.
#
# Whether the IR is *correct* is LlvmRun.cmake's job, by running it.
#
# Inputs: VOLT_BIN, SOURCE (relative to the repo root), WORK (a scratch dir).

get_filename_component( ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE )

if( NOT DEFINED WORK )
  set( WORK ${CMAKE_CURRENT_BINARY_DIR} )
endif()

get_filename_component( STEM ${SOURCE} NAME_WE )
set( ARTIFACT ${WORK}/llvmir-${STEM}.ll )
file( MAKE_DIRECTORY ${WORK} )

#############################################################################

execute_process(
  COMMAND           ${VOLT_BIN} build ${SOURCE} --emit ir -o ${ARTIFACT}
  WORKING_DIRECTORY ${ROOT}
  RESULT_VARIABLE   BUILD_STATUS
  OUTPUT_VARIABLE   BUILD_OUTPUT
  ERROR_VARIABLE    BUILD_ERRORS
)

if( NOT BUILD_STATUS EQUAL 0 )
  message( FATAL_ERROR
    "LlvmIr: `volt build ${SOURCE} --emit ir` failed (${BUILD_STATUS}):\n${BUILD_OUTPUT}${BUILD_ERRORS}" )
endif()

if( NOT EXISTS ${ARTIFACT} )
  message( FATAL_ERROR "LlvmIr: ${SOURCE} reported success but wrote no ${ARTIFACT}" )
endif()

#############################################################################

file( READ ${ARTIFACT} MODULE_TEXT )
if( NOT MODULE_TEXT MATCHES "define[^\n]* @main\\(" )
  message( FATAL_ERROR "LlvmIr: ${SOURCE} emitted no `main` entry point" )
endif()

file( REMOVE ${ARTIFACT} )
