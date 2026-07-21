#############################################################################

set( CMAKE_CXX_STANDARD 26 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

set( CMAKE_C_STANDARD 23 )
set( CMAKE_C_STANDARD_REQUIRED ON )
set( CMAKE_C_EXTENSIONS OFF )

set( CMAKE_EXPORT_COMPILE_COMMANDS ON )

set( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin )
set( CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib )
set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib )

ProcessorCount( NPROC )

#############################################################################

find_program( SCCACHE_FOUND sccache )
find_program( CCACHE_FOUND ccache )
if( SCCACHE_FOUND )
    set( CMAKE_CXX_COMPILER_LAUNCHER sccache )
    set( CMAKE_C_COMPILER_LAUNCHER sccache )
    message( STATUS "[Volt] Compiler launcher: sccache enabled" )
elseif( CCACHE_FOUND )
    set( CMAKE_CXX_COMPILER_LAUNCHER ccache )
    set( CMAKE_C_COMPILER_LAUNCHER ccache )
    message( STATUS "[Volt] Compiler launcher: ccache enabled" )
endif()

#############################################################################

find_program( MOLD_LINKER mold )
find_program( LLD_LINKER  lld )
find_program( GOLD_LINKER gold )

if(MOLD_LINKER)
    message( STATUS "[Volt] Linker: mold (${NPROC} threads)" )
    add_link_options( -fuse-ld=mold -Wl,--threads,--thread-count=${NPROC} )
elseif(LLD_LINKER)
    message( STATUS "[Volt] Linker: lld" )
    add_link_options( -fuse-ld=lld )
elseif(GOLD_LINKER)
    message( STATUS "[Volt] Linker: gold" )
    add_link_options( -fuse-ld=gold )
else()
    message( STATUS "[Volt] Linker: system default" )
endif()

#############################################################################

option( VOLT_ENABLE_TESTING     "Enable Volt test targets"             OFF )
option( VOLT_ENABLE_ASAN        "Enable AddressSanitizer (Debug only)" OFF )
option( VOLT_ENABLE_UBSAN       "Enable UndefinedBehaviorSanitizer"    OFF )
option( VOLT_ENABLE_TSAN        "Enable ThreadSanitizer (Debug only)"   OFF )
option( VOLT_CHECKED_IDS        "Cross-arena TypedId provenance checks (changes Id layout)" OFF )
option( VOLT_UNITY_BUILD        "Enable Unity Build for ultra-fast compilation" ON )
option( VOLT_BUILD_SHARED       "Build internal Volt modules as shared libraries" OFF )

if( VOLT_UNITY_BUILD )
    set( CMAKE_UNITY_BUILD ON )
    set( CMAKE_UNITY_BUILD_BATCH_SIZE 16 )
    message( STATUS "[Volt] Unity Build: enabled (batch size 16)" )
endif()
