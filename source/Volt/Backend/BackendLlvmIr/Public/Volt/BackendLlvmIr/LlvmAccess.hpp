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
// hands the module to ORC, which takes ownership of the module *and* the
// context that typed it, so it moves both out.

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

        // A module and the context it was typed in, kept together because
        // neither outlives the other — exactly what orc::ThreadSafeModule wants.
        struct OwnedModule
        {

            std::unique_ptr<llvm::LLVMContext> Context;
            std::unique_ptr<llvm::Module> Module;
        };

        // Move the finished module out. In Whole granularity that is the one
        // module; in PerUnit it is the prelude. The generator emits nothing
        // afterwards.
        [[nodiscard]] BACKENDLLVMIR_EXPORT OwnedModule TakeModule ( IrGenerator &Gen );

        // The per-unit modules in emission order, which is circuit link order.
        // Empty in Whole granularity.
        [[nodiscard]] BACKENDLLVMIR_EXPORT std::vector<OwnedModule> TakeUnitModules ( IrGenerator &Gen );

        // Non-owning borrows, for the AOT tail that verifies, optimises and
        // emits over the module the generator still owns.
        [[nodiscard]] BACKENDLLVMIR_EXPORT llvm::Module &ModuleOf ( IrGenerator &Gen );

        // Null whenever the options asked for no TargetMachine — the JIT path.
        [[nodiscard]] BACKENDLLVMIR_EXPORT llvm::TargetMachine *MachineOf ( IrGenerator &Gen );

    } // namespace Ir

} // namespace Backend

} // namespace Volt
