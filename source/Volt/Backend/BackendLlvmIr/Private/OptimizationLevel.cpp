// OptimizationLevel.cpp — see OptimizationLevel.hpp.

#include "Volt/BackendLlvmIr/OptimizationLevel.hpp"

llvm::OptimizationLevel Volt::Backend::Ir::OptimizationLevelOf ( const std::uint8_t OptLevel )
{
    if ( OptLevel >= 3 )
    {
        return llvm::OptimizationLevel::O3;
    }
    if ( OptLevel == 2 )
    {
        return llvm::OptimizationLevel::O2;
    }
    if ( OptLevel == 1 )
    {
        return llvm::OptimizationLevel::O1;
    }
    return llvm::OptimizationLevel::O0;
}
