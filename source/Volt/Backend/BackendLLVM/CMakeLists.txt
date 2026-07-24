# Optional module: only built when the LLVM toolchain is opted in. Nothing
# else links it, so an early return simply leaves the target undefined.
if( NOT VOLT_ENABLE_LLVM )
  return()
endif()

find_package( LLVM REQUIRED CONFIG )
message( STATUS "[Volt] LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}" )

VoltModule(
  NAME            BackendLLVM
  TYPE            shared_library
  SOURCES         "Private/*.cpp"
  PUBLIC_INCLUDES "Public/"
  PRIVATE_INCLUDES "Private/"
  DEPS            BackendCore
)

# LLVM headers are third-party: SYSTEM keeps them outside -Werror's blast
# radius. Prefer the monolithic dylib when this LLVM provides one.
target_include_directories( BackendLLVM SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} )
if( TARGET LLVM )
  target_link_libraries( BackendLLVM PRIVATE LLVM )
else()
  llvm_map_components_to_libnames( VOLT_LLVM_LIBS core support )
  target_link_libraries( BackendLLVM PRIVATE ${VOLT_LLVM_LIBS} )
endif()
