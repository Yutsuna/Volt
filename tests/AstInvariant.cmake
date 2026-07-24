# AstInvariant.cmake — the corpus-wide replay of the two AST contracts.
#
# The AstInvariant pass (Sema, order 40) already reports both as hard errors,
# per file, wherever `volt check` runs. This test is the other half: it replays
# them over the *whole* tree, so a regression cannot hide in a sample that no
# Check.* test happens to cover.
#
#   1. No residual sugar, everywhere. `parse --lowered` runs exactly the
#      Lowering passes, which is all the first contract needs, so this half
#      applies to every sample and every stdlib file regardless of whether it
#      type-checks. The node list is read out of Nodes.inl — the manifest is
#      the single source of truth here too, so a tenth VOLT_EXPR_SUGAR is
#      covered the day it is written.
#
#   2. Typing is total over source/Lib. `check` must succeed there; the pass
#      turns any untyped value expression into an error, so a green run is the
#      assertion. samples/Sema is covered the same way by the Check.* tests.
#
# Not covered on purpose: samples outside samples/Sema are parse fixtures and
# several of them do not type-check for reasons that predate this contract —
# `Array#length`, `puts` and `Hash#each` do not exist in the stdlib, and
# FunctionalSpec.vl leans on unannotated lambda parameters, which PLAN §VI.5
# refuses by design. Those are stdlib and language gaps, not invariant
# breaches; nothing here silences them, they simply have no check test yet.

if( NOT DEFINED VOLT_BIN )
  message( FATAL_ERROR "AstInvariant: VOLT_BIN is required" )
endif()

get_filename_component( VOLT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE )

#############################################################################
# The sugar node names, straight from the manifest.

file( READ "${VOLT_ROOT}/source/Volt/Frontend/Public/Volt/Frontend/AST/Nodes.inl" NODES_INL )
string( REGEX MATCHALL "VOLT_EXPR_SUGAR[ \t]*\\([ \t]*[A-Za-z_]+" SUGAR_MATCHES "${NODES_INL}" )

set( SUGAR_NODES "" )
foreach( MATCH IN LISTS SUGAR_MATCHES )
  string( REGEX REPLACE "^VOLT_EXPR_SUGAR[ \t]*\\([ \t]*" "" NAME "${MATCH}" )
  # The `#define VOLT_EXPR_SUGAR( Name ) ...` fallback line matches too.
  if( NOT NAME STREQUAL "Name" )
    list( APPEND SUGAR_NODES "${NAME}" )
  endif()
endforeach()
list( REMOVE_DUPLICATES SUGAR_NODES )

if( SUGAR_NODES STREQUAL "" )
  message( FATAL_ERROR "AstInvariant: no VOLT_EXPR_SUGAR found in Nodes.inl — the manifest read is broken" )
endif()
list( LENGTH SUGAR_NODES SUGAR_COUNT )
message( STATUS "AstInvariant: ${SUGAR_COUNT} sugar node kind(s) from the manifest: ${SUGAR_NODES}" )

#############################################################################
# 1. No residual sugar, over every sample and every stdlib file.

file( GLOB_RECURSE INPUTS RELATIVE ${VOLT_ROOT}
  ${VOLT_ROOT}/samples/*.vl ${VOLT_ROOT}/samples/*.vlx
  ${VOLT_ROOT}/source/Lib/*.vl ${VOLT_ROOT}/source/Lib/*.vlx )
list( SORT INPUTS )

set( FAILURES "" )
foreach( INPUT IN LISTS INPUTS )
  execute_process(
    COMMAND           ${VOLT_BIN} parse --lowered --no-color --no-location -i ${INPUT}
    WORKING_DIRECTORY ${VOLT_ROOT}
    OUTPUT_VARIABLE   TREE
    ERROR_VARIABLE    TREE_ERR
    RESULT_VARIABLE   TREE_RC )
  if( NOT TREE_RC EQUAL 0 )
    # A lowering pass may *refuse* a file — CompoundAssignReceiver.vl exists
    # precisely to be refused. A refusal is a diagnostic and leaves no tree to
    # census; anything else (a crash, a missing binary) is a real failure.
    if( NOT TREE_ERR MATCHES "error:" )
      list( APPEND FAILURES "${INPUT}: `parse --lowered` failed with no diagnostic\n${TREE_ERR}" )
    endif()
    continue()
  endif()
  foreach( NODE IN LISTS SUGAR_NODES )
    # The dumper writes "├─ Interp" / "└─ Interp"; anchoring on the branch
    # glyph keeps a field named after a node from matching.
    if( TREE MATCHES "─ ${NODE}\n" OR TREE MATCHES "─ ${NODE} " )
      list( APPEND FAILURES "${INPUT}: residual sugar node ${NODE} survived lowering" )
    endif()
  endforeach()
endforeach()

#############################################################################
# 2. Typing is total over the standard library.

execute_process(
  COMMAND           ${VOLT_BIN} check source/Lib
  WORKING_DIRECTORY ${VOLT_ROOT}
  OUTPUT_VARIABLE   CHECK_OUT
  ERROR_VARIABLE    CHECK_ERR
  RESULT_VARIABLE   CHECK_RC )
if( NOT CHECK_RC EQUAL 0 )
  list( APPEND FAILURES "source/Lib: `check` failed\n${CHECK_ERR}" )
endif()

#############################################################################

if( NOT FAILURES STREQUAL "" )
  string( REPLACE ";" "\n" REPORT "${FAILURES}" )
  message( FATAL_ERROR "AstInvariant: the AST contract is broken\n${REPORT}" )
endif()

list( LENGTH INPUTS INPUT_COUNT )
message( STATUS "AstInvariant: OK — ${INPUT_COUNT} file(s), no residual sugar, source/Lib fully typed" )
