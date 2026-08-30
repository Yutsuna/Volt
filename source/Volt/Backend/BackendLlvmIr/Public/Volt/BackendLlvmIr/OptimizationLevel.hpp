#pragma once

// OptimizationLevel.hpp — what `-O<n>` means, once.
//
// LLVM-AWARE CONSUMERS ONLY, like LlvmAccess.hpp beside it, and for the same
// reason: `llvm::OptimizationLevel` is an LLVM type, and the Driver has no
// business seeing one.
//
// This is here rather than in either tail because `volt build -O2` and
// `volt run -O2` are the same promise to the user. The two tails compose
// *different pipelines* around the answer — the AOT one internalises and DCEs
// first, because its module is a whole program and the JIT's are fragments of
// one — but the level they build that pipeline for is not a thing either of
// them gets to decide on its own.

#include "BackendLlvmIr_export.hpp"

#include <llvm/Passes/OptimizationLevel.h>

#include <cstdint>

namespace llvm
{
class Function;
class Module;
class TargetMachine;
} // namespace llvm

namespace Volt
{

namespace Backend
{

    namespace Ir
    {

        // Anything above 3 saturates rather than being an error: the CLI has
        // already rejected what it does not accept, and a backend given a level
        // it does not know should compile the program, not refuse it.
        [[nodiscard]] BACKENDLLVMIR_EXPORT llvm::OptimizationLevel OptimizationLevelOf ( std::uint8_t OptLevel );

        // Run PassBuilder's default pipeline for `Level` over `Mod`.
        BACKENDLLVMIR_EXPORT void
        RunOptimizationPipeline ( llvm::Module &Mod, llvm::OptimizationLevel Level, llvm::TargetMachine *Machine = nullptr );

        // Check whether a function contains any loops (CFG back-edges).
        [[nodiscard]] BACKENDLLVMIR_EXPORT bool HasLoop ( const llvm::Function &Fn );

        // Whether a function has sufficient complexity (loops or substantial instruction count)
        // to benefit from higher-tier optimization passes.
        [[nodiscard]] BACKENDLLVMIR_EXPORT bool IsCandidateForOptimization ( const llvm::Function &Fn );

    } // namespace Ir

} // namespace Backend

} // namespace Volt
