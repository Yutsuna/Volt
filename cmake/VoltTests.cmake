# VoltTests.cmake — ctest registration, gated behind VOLT_ENABLE_TESTING
# (`volt-build testing`). Included after VoltBuild so the Volt target exists.
#
# Three suites, all driven from the source tree:
#   Golden.*      `volt parse` output of every samples/** file compared to
#                 tests/golden/ (regenerate via the golden-update target, or
#                 `cmake -DUPDATE=1 -P tests/GoldenTest.cmake`).
#   Corpus.*      every stdlib file under source/Lib/** must parse clean.
#   ZeroHardcode  no Volt type name leaks into Frontend/ or Sema/ C++.

if( NOT VOLT_ENABLE_TESTING )
  return()
endif()

enable_testing()

set( VOLT_ROOT ${CMAKE_CURRENT_SOURCE_DIR} )

file( GLOB_RECURSE VOLT_GOLDEN_SAMPLES RELATIVE ${VOLT_ROOT} CONFIGURE_DEPENDS
  ${VOLT_ROOT}/samples/*.vl ${VOLT_ROOT}/samples/*.vlx )
list( SORT VOLT_GOLDEN_SAMPLES )
foreach( SAMPLE IN LISTS VOLT_GOLDEN_SAMPLES )
  add_test(
    NAME    Golden.${SAMPLE}
    COMMAND ${CMAKE_COMMAND}
            -DVOLT_BIN=$<TARGET_FILE:Volt>
            -DSOURCE=${SAMPLE}
            -P ${VOLT_ROOT}/tests/GoldenTest.cmake
  )
endforeach()

file( GLOB_RECURSE VOLT_CORPUS_SOURCES RELATIVE ${VOLT_ROOT} CONFIGURE_DEPENDS
  ${VOLT_ROOT}/source/Lib/*.vl ${VOLT_ROOT}/source/Lib/*.vlx )
list( SORT VOLT_CORPUS_SOURCES )
foreach( SOURCE IN LISTS VOLT_CORPUS_SOURCES )
  add_test(
    NAME              Corpus.${SOURCE}
    COMMAND           $<TARGET_FILE:Volt> parse --no-color -i ${SOURCE}
    WORKING_DIRECTORY ${VOLT_ROOT}
  )
endforeach()

add_test(
  NAME    ZeroHardcode
  COMMAND ${CMAKE_COMMAND} -P ${VOLT_ROOT}/tests/ZeroHardcode.cmake
)

add_custom_target( golden-update
  COMMAND ${CMAKE_COMMAND} -DVOLT_BIN=$<TARGET_FILE:Volt> -DUPDATE=1
          -P ${VOLT_ROOT}/tests/GoldenTest.cmake
  COMMENT "Regenerating tests/golden/ from `volt parse`"
)
add_dependencies( golden-update Volt )
