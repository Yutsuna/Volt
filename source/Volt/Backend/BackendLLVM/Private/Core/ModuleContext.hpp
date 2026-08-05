#pragma once

// ModuleContext.hpp — the owned LLVM state: context, module, builder, target.
//
// This is the one private header that includes real LLVM headers rather than
// LlvmFwd.hpp, and it is deliberate: it holds an `llvm::LLVMContext` by value
// and a `std::unique_ptr<llvm::IRBuilder<>>`, neither of which a forward
// declaration can express. Everything else under Private/ forward-declares
// instead (see LlvmFwd.hpp).

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <string_view>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class ModuleContext
        {

        public:

            // Create the module and the host TargetMachine. Separate from the
            // constructor because the triple is a *build* decision, and the seam
            // for cross-compilation is exactly this one string. False on
            // failure, with `OutError` naming why.
            [[nodiscard]] bool InitTarget ( std::string_view ModuleName, std::string &OutError );

            [[nodiscard]] llvm::LLVMContext &Context () noexcept
            {
                return Ctx;
            }

            [[nodiscard]] llvm::Module &Mod () noexcept
            {
                return *Module;
            }

            [[nodiscard]] llvm::Module *ModPtr () noexcept
            {
                return Module.get();
            }

            [[nodiscard]] llvm::IRBuilder<> &Builder () noexcept
            {
                return *Build;
            }

            [[nodiscard]] llvm::TargetMachine &Machine () noexcept
            {
                return *Target;
            }

            [[nodiscard]] llvm::TargetMachine *MachinePtr () noexcept
            {
                return Target.get();
            }

            [[nodiscard]] bool Ready () const noexcept
            {
                return Module != nullptr and Build != nullptr;
            }

            // True when the block being written into already ends in a
            // terminator, so a walk stops appending instructions after a
            // `return` / `break`.
            [[nodiscard]] bool Terminated () const;

        private:

            llvm::LLVMContext Ctx;
            std::unique_ptr<llvm::Module> Module;
            std::unique_ptr<llvm::IRBuilder<>> Build;
            std::unique_ptr<llvm::TargetMachine> Target;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
