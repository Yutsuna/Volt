#############################################################################

set( CMAKE_CXX_STANDARD 23 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

set( CMAKE_C_STANDARD 23 )
set( CMAKE_C_STANDARD_REQUIRED ON )
set( CMAKE_C_EXTENSIONS OFF )

set( CMAKE_EXPORT_COMPILE_COMMANDS ON )

ProcessorCount( NPROC )

#############################################################################

find_program( CCACHE_FOUND ccache )
if( CCACHE_FOUND )
    set( CMAKE_CXX_COMPILER_LAUNCHER ccache )
    message( STATUS "[Volt] Compiler: ccache enabled" )
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
