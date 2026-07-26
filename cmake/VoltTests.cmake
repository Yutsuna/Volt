if( NOT VOLT_ENABLE_TESTING )
  return()
endif()

enable_testing()

#############################################################################

set( VOLT_ROOT ${CMAKE_CURRENT_SOURCE_DIR} )

file( GLOB_RECURSE VOLT_GOLDEN_SAMPLES RELATIVE ${VOLT_ROOT} CONFIGURE_DEPENDS
  ${VOLT_ROOT}/samples/*.vl ${VOLT_ROOT}/samples/*.vlx )
list( SORT VOLT_GOLDEN_SAMPLES )

#############################################################################

foreach( SAMPLE IN LISTS VOLT_GOLDEN_SAMPLES )
  add_test(
    NAME    Golden.${SAMPLE}
    COMMAND ${CMAKE_COMMAND}
            -DVOLT_BIN=$<TARGET_FILE:Volt>
            -DSOURCE=${SAMPLE}
            -P ${VOLT_ROOT}/tests/GoldenTest.cmake
  )
  add_test(
    NAME    Golden.lowered.${SAMPLE}
    COMMAND ${CMAKE_COMMAND}
            -DVOLT_BIN=$<TARGET_FILE:Volt>
            -DSOURCE=${SAMPLE}
            -DLOWERED=1
            -P ${VOLT_ROOT}/tests/GoldenTest.cmake
  )
endforeach()

#############################################################################

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

#############################################################################

# Sema samples run the full check pipeline (parse + interfaces + passes),
# not just parse: scoping/typing diagnostics only exist there. A sample
# listed in VOLT_CHECK_EXPECT_FAIL asserts that check *rejects* it.
file( GLOB_RECURSE VOLT_CHECK_SOURCES RELATIVE ${VOLT_ROOT} CONFIGURE_DEPENDS
  ${VOLT_ROOT}/samples/Sema/*.vl )
list( SORT VOLT_CHECK_SOURCES )

set( VOLT_CHECK_EXPECT_FAIL
  samples/Sema/AbstractConformance.vl
  samples/Sema/AssignMismatchAssign.vl
  samples/Sema/AssignMismatchDefault.vl
  samples/Sema/AssignMismatchLocal.vl
  samples/Sema/AssignMismatchReturn.vl
  samples/Sema/AssignMismatchTrailing.vl
  samples/Sema/CompoundAssignReceiver.vl
  samples/Sema/NilableUnsupported.vl
  samples/Sema/DotCallNoReceiver.vl
  samples/Sema/FreeFunctionArityMismatch.vl
  samples/Sema/FreeFunctionTypeMismatch.vl
  samples/Sema/InterpNoToString.vl
  samples/Sema/NonExhaustiveCase.vl
  samples/Sema/RedeclareSameScope.vl
  samples/Sema/UnknownFreeFunction.vl
  samples/Sema/UnknownMember.vl )

foreach( SOURCE IN LISTS VOLT_CHECK_SOURCES )
  add_test(
    NAME              Check.${SOURCE}
    COMMAND           $<TARGET_FILE:Volt> check -i ${SOURCE}
    WORKING_DIRECTORY ${VOLT_ROOT}
  )
  if( SOURCE IN_LIST VOLT_CHECK_EXPECT_FAIL )
    set_tests_properties( Check.${SOURCE} PROPERTIES WILL_FAIL TRUE )
  endif()
endforeach()

#############################################################################

# The native backend's own corpus: every sample is compiled to a real
# executable and run, and its exit code compared against the `.expected` file
# beside it. Only registered when the backend exists — `volt build` reports
# "configured without LLVM" otherwise, and a test asserting that would be
# asserting the absence of the thing under test.
if( VOLT_ENABLE_LLVM )
  file( GLOB VOLT_CODEGEN_SOURCES RELATIVE ${VOLT_ROOT} CONFIGURE_DEPENDS
    ${VOLT_ROOT}/samples/Codegen/*.vl )
  list( SORT VOLT_CODEGEN_SOURCES )

  foreach( SOURCE IN LISTS VOLT_CODEGEN_SOURCES )
    add_test(
      NAME    LlvmIr.${SOURCE}
      COMMAND ${CMAKE_COMMAND}
              -DVOLT_BIN=$<TARGET_FILE:Volt>
              -DSOURCE=${SOURCE}
              -DWORK=${CMAKE_BINARY_DIR}/tests
              -P ${VOLT_ROOT}/tests/LlvmIr.cmake
    )
    add_test(
      NAME    LlvmRun.${SOURCE}
      COMMAND ${CMAKE_COMMAND}
              -DVOLT_BIN=$<TARGET_FILE:Volt>
              -DSOURCE=${SOURCE}
              -DWORK=${CMAKE_BINARY_DIR}/tests
              -P ${VOLT_ROOT}/tests/LlvmRun.cmake
    )
  endforeach()
endif()

#############################################################################

add_test(
  NAME    ZeroHardcode
  COMMAND ${CMAKE_COMMAND} -P ${VOLT_ROOT}/tests/ZeroHardcode.cmake
)

# The two AST contracts of rules/core-ast.md, replayed over the whole tree:
# no residual sugar anywhere, and total typing over source/Lib.
add_test(
  NAME    AstInvariant
  COMMAND ${CMAKE_COMMAND} -DVOLT_BIN=$<TARGET_FILE:Volt>
          -P ${VOLT_ROOT}/tests/AstInvariant.cmake
)

add_custom_target( golden-update
  COMMAND ${CMAKE_COMMAND} -DVOLT_BIN=$<TARGET_FILE:Volt> -DUPDATE=1
          -P ${VOLT_ROOT}/tests/GoldenTest.cmake
  COMMENT "Regenerating tests/golden/ from `volt parse`"
)
add_dependencies( golden-update Volt )
