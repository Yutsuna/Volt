function( VoltRegisterPackage NAME TARGET PKGCONF )
  string( TOUPPER "${NAME}" NAME_UPPER )
  set_property( GLOBAL PROPERTY VOLT_PKG_${NAME_UPPER}_TARGET
    "${TARGET}"
  )
  set_property( GLOBAL PROPERTY VOLT_PKG_${NAME_UPPER}_PKGCONF
    "${PKGCONF}"
  )
endfunction()

#############################################################################

VoltRegisterPackage(
  LLVM
  LLVM::LLVM
  "llvm"
)
