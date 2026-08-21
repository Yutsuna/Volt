#pragma once

// ModuleContext.hpp — the owned LLVM state: context, module, builder, target.
//
// This is the one private header that includes real LLVM headers rather than
// LlvmFwd.hpp, and it is deliberate: it owns an `llvm::IRBuilder<>` and hands
// out `llvm::LLVMContext`, neither of which a forward declaration can express.
// Everything else under Private/ forward-declares instead (see LlvmFwd.hpp).
//
// The context is held by *pointer*, not by value, for one reason: ORC's
// `orc::ThreadSafeModule` takes ownership of both the module and the context
// that typed it, so a JIT consumer has to be able to move both out of here.
// An AOT consumer never exercises that, but paying for a `unique_ptr`
// indirection on a once-per-build object is not a cost worth two shapes.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <string>
#include <string_view>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        // Which machine the IR is being typed for. The AOT build derives all
        // three from the host; a JIT is *told* its triple and layout by LLJIT
        // and wants no TargetMachine at all, because it never runs
        // addPassesToEmitFile.
        struct TargetSpec
        {

            std::string Triple;             // empty -> llvm::sys::getDefaultTargetTriple()
            std::string DataLayout;         // empty -> taken from the created TargetMachine
            bool bNeedTargetMachine = true; // AOT: true. JIT: false.
        };

        class ModuleContext
        {

        public:

            // Create the module and, when the spec asks for one, the
            // TargetMachine. Separate from the constructor because the triple is
            // a *build* decision, and the seam for cross-compilation is exactly
            // that one string. False on failure, with `OutError` naming why.
            [[nodiscard]] bool InitTarget ( std::string_view ModuleName, const TargetSpec &Spec, std::string &OutError );

            [[nodiscard]] llvm::LLVMContext &Context () noexcept
            {
                return *Ctx;
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

            // Null whenever the spec asked for no TargetMachine — the JIT path.
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

            // Hand both halves to a consumer that must own them together — the
            // ThreadSafeModule case. The context is emptied along with the
            // module, so nothing may be emitted through this object afterwards.
            [[nodiscard]] std::unique_ptr<llvm::Module> TakeModule () noexcept
            {
                Build.reset();
                return std::move( Module );
            }

            [[nodiscard]] std::unique_ptr<llvm::LLVMContext> TakeContext () noexcept
            {
                return std::move( Ctx );
            }

        private:

            std::unique_ptr<llvm::LLVMContext> Ctx;
            std::unique_ptr<llvm::Module> Module;
            std::unique_ptr<llvm::IRBuilder<>> Build;
            std::unique_ptr<llvm::TargetMachine> Target;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
