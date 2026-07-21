cmake_minimum_required(VERSION 4.0)

project(
  Volt
  VERSION 0.1.0
  DESCRIPTION "Volt is a compiled and interpreted programming language"
  LANGUAGES CXX C
)

list( APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" )

include( ProcessorCount )
include(GenerateExportHeader)

include( VoltTooling )
include( VoltFormat )
include( VoltTidy )
include( VoltOptions )
include( VoltCompiler )
include( VoltLLVM )
include( VoltModule )
include( VoltBuild )
include( VoltTests )
