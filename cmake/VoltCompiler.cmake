###########################################################

add_library( VoltCompileOptions INTERFACE )
add_library( Volt::CompileOptions ALIAS VoltCompileOptions )

###########################################################

set( CMAKE_CXX_SCAN_FOR_MODULES OFF )

###########################################################

target_compile_options( VoltCompileOptions INTERFACE
  $<$<CXX_COMPILER_ID:GNU>:-freflection>
 )

target_compile_options( VoltCompileOptions INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Wcast-align
  -Wunused
  -Woverloaded-virtual
  -Wconversion
  -Wsign-conversion
  -Wnull-dereference
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
  -Wno-missing-braces
  -Werror
 )

target_compile_options( VoltCompileOptions INTERFACE
  -fvisibility=hidden
  -fvisibility-inlines-hidden
 )

target_compile_options( VoltCompileOptions INTERFACE
  $<$<CONFIG:Debug>:-Og;-g3;-fno-omit-frame-pointer>
  $<$<CONFIG:Release>:-O3;-DNDEBUG>
  $<$<CONFIG:RelWithDebInfo>:-O2;-g;-DNDEBUG>
 )

set( VOLT_PCH_HEADERS
  <vector>
  <string>
  <string_view>
  <memory>
  <utility>
  <cstdint>
  <cstddef>
  <algorithm>
  <functional>
  <variant>
  <optional>
  <unordered_map>
  <span>
  <concepts>
  <type_traits>
  <initializer_list>
  <array>
)

###########################################################

if( VOLT_CHECKED_IDS )
  target_compile_definitions( VoltCompileOptions INTERFACE VOLT_CHECKED_IDS=1 )
  message( STATUS "[Volt] Checked IDs: enabled" )
endif()

if( VOLT_ENABLE_ASAN )
  target_compile_options( VoltCompileOptions INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address;-fno-omit-frame-pointer>
   )
  target_link_options( VoltCompileOptions INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address>
   )
   message( STATUS "[Volt] ASan: enabled" )
endif()

if( VOLT_ENABLE_UBSAN )
  target_compile_options( VoltCompileOptions INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=undefined>
   )
  target_link_options( VoltCompileOptions INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=undefined>
   )
   message( STATUS "[Volt] UBSan: enabled" )
endif()

if( VOLT_ENABLE_TSAN )
  target_compile_options( VoltCompileOptions INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=thread;-fno-omit-frame-pointer>
   )
  target_link_options( VoltCompileOptions INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=thread>
   )
   message( STATUS "[Volt] TSan: enabled" )
endif()

###########################################################

option( VOLT_ENABLE_LTO "Enable Link-Time Optimisation in Release" OFF )
if( VOLT_ENABLE_LTO )
  include( CheckIPOSupported )
  check_ipo_supported( RESULT IPO_OK OUTPUT IPO_ERR )
  if( IPO_OK )
    set_property( TARGET VoltCompileOptions PROPERTY
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE )
    message( STATUS "[Volt] LTO: enabled" )
  else()
    message( WARNING "[Volt] LTO requested but not supported: ${IPO_ERR}" )
  endif()
endif()

###########################################################
