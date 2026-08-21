// ModuleLocal.cpp — see ModuleLocal.hpp.

#include "Core/ModuleLocal.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"

#include <llvm/IR/Module.h>

llvm::Function *Volt::Backend::Llvm::LocalCopy ( EmitterServices &Services, llvm::Function *Fn )
{
    if ( Fn == nullptr or Services.Ctx == nullptr )
    {
        return Fn;
    }

    llvm::Module &Mod = Services.Ctx->Mod();
    if ( Fn->getParent() == &Mod )
    {
        return Fn;
    }

    if ( llvm::Function *Here = Mod.getFunction( Fn->getName() ); Here != nullptr )
    {
        return Here;
    }

    llvm::Function *Here = llvm::Function::Create( Fn->getFunctionType(), llvm::Function::ExternalLinkage, Fn->getName(), &Mod );

    // Carried over because they are part of how the callee is *called*, not of
    // how it is defined: an sret or byval parameter attribute that appeared on
    // the definition but not on this declaration would make the two disagree
    // about the ABI at the call site.
    Here->setAttributes( Fn->getAttributes() );
    Here->setCallingConv( Fn->getCallingConv() );
    return Here;
}

llvm::GlobalVariable *Volt::Backend::Llvm::LocalCopy ( EmitterServices &Services, llvm::GlobalVariable *Var )
{
    if ( Var == nullptr or Services.Ctx == nullptr )
    {
        return Var;
    }

    llvm::Module &Mod = Services.Ctx->Mod();
    if ( Var->getParent() == &Mod )
    {
        return Var;
    }

    if ( llvm::GlobalVariable *Here = Mod.getGlobalVariable( Var->getName(), /*AllowInternal=*/true ); Here != nullptr )
    {
        return Here;
    }

    // No initialiser: that is what makes it a declaration rather than a second
    // definition of the same storage.
    auto *Here = new llvm::GlobalVariable( Mod, Var->getValueType(), Var->isConstant(), llvm::GlobalValue::ExternalLinkage,
                                           nullptr, Var->getName(), nullptr, Var->getThreadLocalMode() );
    Here->setAlignment( Var->getAlign().valueOrOne() );
    return Here;
}
