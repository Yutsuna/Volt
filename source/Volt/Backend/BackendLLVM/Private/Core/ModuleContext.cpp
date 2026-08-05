// ModuleContext.cpp — module + host TargetMachine creation.

#include "Core/ModuleContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <mutex>
#include <string>

namespace
{

// LLVM's target registry is process-global and not re-entrant. Volt compiles
// units in parallel elsewhere, so the initialisation is guarded even though
// codegen itself is single-threaded.
void InitialiseNativeTarget ()
{
    static std::once_flag Once;
    std::call_once( Once,
                    [] ()
                    {
                        llvm::InitializeNativeTarget();
                        llvm::InitializeNativeTargetAsmPrinter();
                        llvm::InitializeNativeTargetAsmParser();
                    } );
}

} // namespace

bool Volt::Backend::Llvm::ModuleContext::InitTarget ( std::string_view ModuleName, std::string &OutError )
{
    InitialiseNativeTarget();

    Module = std::make_unique<llvm::Module>( ModuleName, Ctx );
    Build  = std::make_unique<llvm::IRBuilder<>>( Ctx );

    // One string is the whole cross-compilation seam.
    const std::string TripleText = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple Triple{ TripleText };
    Module->setTargetTriple( Triple );

    std::string Error;
    const llvm::Target *Found = llvm::TargetRegistry::lookupTarget( Triple, Error );
    if ( Found == nullptr )
    {
        OutError = "llvm: no target for host triple '" + TripleText + "': " + Error;
        return false;
    }

    // PIC, not the default static model: every mainstream toolchain links PIE
    // by default, and a static-model object hits `R_X86_64_32 ... can not be
    // used; recompile with -fPIC` at the link, which is a failure with no
    // relation to anything in the program. The C driver LinkerDriver.cpp shells
    // out to is the same one that made that choice, so matching it here is what
    // keeps the two halves of the build agreeing.
    Target.reset( Found->createTargetMachine( Triple, "generic", "", llvm::TargetOptions{}, llvm::Reloc::PIC_ ) );
    if ( Target == nullptr )
    {
        OutError = "llvm: could not create a TargetMachine for '" + TripleText + "'";
        return false;
    }

    // Set before any type is created: the ABI cross-check in AbiVerifier reads
    // struct offsets out of this DataLayout and compares them against
    // LayoutEngine, so an unset layout would make the check vacuous.
    Module->setDataLayout( Target->createDataLayout() );
    return true;
}

bool Volt::Backend::Llvm::ModuleContext::Terminated () const
{
    if ( Build == nullptr )
    {
        return true;
    }
    llvm::BasicBlock *Block = Build->GetInsertBlock();
    return Block == nullptr or Block->getTerminator() != nullptr;
}
