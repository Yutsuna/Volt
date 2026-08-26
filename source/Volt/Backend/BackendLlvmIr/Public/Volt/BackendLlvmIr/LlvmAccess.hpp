#pragma once

// LlvmAccess.hpp — the LLVM-typed half of IrGenerator's surface.
//
// LLVM-AWARE CONSUMERS ONLY: BackendLLVM and BackendJIT. It is a separate
// header from IrGenerator.hpp for one reason, and it is the same reason
// Public/Volt/BackendLLVM/LlvmEmitter.hpp mentions no llvm:: type — so that the
// Driver, and anything else that only needs to *drive* an emission, can include
// the interface without an LLVM header appearing anywhere in its build
// (rules/shared-lib-exports.md).
//
// Two shapes of access, because the two tails want opposite things: the AOT
// tail optimises and emits the module in place, so it borrows; the JIT tail
// hands the modules to ORC, which takes ownership of them *and* of the context
// that typed them, so it moves everything out.

#include "BackendLlvmIr_export.hpp"

#include "Volt/BackendLlvmIr/IrGenerator.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Ir
    {

        // Everything the emission produced, and the context that types it.
        //
        // One context for the batch, not one each: llvm::Type is context-owned,
        // so modules of a single build have to share one or they cannot refer to
        // each other's functions by signature at all. ORC's unit of ownership is
        // exactly this pair — a ThreadSafeContext, and the modules opened in it.
        //
        // `Modules` is in emission order, which is: the declarations the build
        // opens with, then one module per unit in circuit link order, then the
        // shared module Finish caps the emission with (monomorphisations,
        // `_V_init_all`, the symbol table, the entry point). Whole granularity
        // yields exactly one entry, and it is that last one.
        struct OwnedModules
        {

            std::unique_ptr<llvm::LLVMContext> Context;
            std::vector<std::unique_ptr<llvm::Module>> Modules;
        };

        // Move the finished emission out. The generator emits nothing
        // afterwards — the context leaves with it.
        [[nodiscard]] BACKENDLLVMIR_EXPORT OwnedModules TakeModules ( IrGenerator &Gen );

        // Non-owning borrows, for the AOT tail that verifies, optimises and
        // emits over the module the generator still owns.
        [[nodiscard]] BACKENDLLVMIR_EXPORT llvm::Module &ModuleOf ( IrGenerator &Gen );

        // Null whenever the options asked for no TargetMachine — the JIT path.
        [[nodiscard]] BACKENDLLVMIR_EXPORT llvm::TargetMachine *MachineOf ( IrGenerator &Gen );

    } // namespace Ir

} // namespace Backend

} // namespace Volt
