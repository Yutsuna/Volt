// FunctionSlots.cpp — see FunctionSlots.hpp.

#include "Functions/FunctionSlots.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Functions/FunctionRegistry.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace
{

using Volt::Backend::Llvm::EmitterServices;

bool IndirectLinkage ( const EmitterServices &Services )
{
    return Services.Options != nullptr and Services.Options->Linkage == Volt::Backend::Ir::ELinkage::Indirect;
}

// Find-or-create, without deciding whether this module defines the body: both
// SlotFor and DefineLocalSlots want the same object, and which of them reaches
// it first depends on emission order, not on meaning.
llvm::GlobalVariable *SlotIn ( llvm::Module &Mod, llvm::StringRef Symbol )
{
    const std::string Name = Volt::Backend::Ir::SlotNameOf( Symbol );
    if ( llvm::GlobalVariable *Here = Mod.getGlobalVariable( Name, /*AllowInternal=*/true ); Here != nullptr )
    {
        return Here;
    }
    return new llvm::GlobalVariable( Mod, llvm::PointerType::getUnqual( Mod.getContext() ), /*isConstant=*/false,
                                     llvm::GlobalValue::ExternalLinkage, /*Initializer=*/nullptr, Name );
}

} // namespace

llvm::GlobalVariable *Volt::Backend::Llvm::SlotFor ( EmitterServices &Services, llvm::Function *Fn )
{
    if ( Fn == nullptr or Services.Ctx == nullptr or Services.Functions == nullptr or not IndirectLinkage( Services ) )
    {
        return nullptr;
    }
    if ( not Services.Functions->IsIndirectable( Fn->getName().str() ) )
    {
        return nullptr;
    }
    return SlotIn( Services.Ctx->Mod(), Fn->getName() );
}

llvm::Value *Volt::Backend::Llvm::CalleeValue ( EmitterServices &Services, llvm::Function *Fn )
{
    llvm::GlobalVariable *Slot = SlotFor( Services, Fn );
    if ( Slot == nullptr )
    {
        return Fn;
    }

    // One load per call, deliberately not hoisted by anything here: the whole
    // point is that the address is read at the moment of the call, so a reload
    // between two executions of the same call site is seen by the second.
    return Services.Ctx->Builder().CreateLoad( llvm::PointerType::getUnqual( Services.Ctx->Mod().getContext() ), Slot,
                                               Fn->getName() + ".slot" );
}

void Volt::Backend::Llvm::DefineLocalSlots ( EmitterServices &Services )
{
    if ( Services.Ctx == nullptr or Services.Functions == nullptr or not IndirectLinkage( Services ) )
    {
        return;
    }

    // A replacement defines no slot. The slots belong to the emission that is
    // already running — they are how it will reach this new code — and a second
    // definition of one would be a second, unpatched copy.
    if ( Services.Options->bReplaceUnit )
    {
        return;
    }

    llvm::Module &Mod = Services.Ctx->Mod();
    for ( llvm::Function &Fn : Mod )
    {
        if ( Fn.isDeclaration() or not Services.Functions->IsIndirectable( Fn.getName().str() ) )
        {
            continue;
        }
        llvm::GlobalVariable *Slot = SlotIn( Mod, Fn.getName() );
        if ( Slot->hasInitializer() )
        {
            continue;
        }
        Slot->setInitializer( &Fn );
    }
}

std::vector<Volt::Backend::Ir::IrGenerator::UnitSymbol> Volt::Backend::Llvm::LocalDefinedSymbols ( EmitterServices &Services )
{
    std::vector<Ir::IrGenerator::UnitSymbol> Out;
    if ( Services.Ctx == nullptr or Services.Functions == nullptr )
    {
        return Out;
    }
    for ( llvm::Function &Fn : Services.Ctx->Mod() )
    {
        if ( Fn.isDeclaration() or not Services.Functions->IsIndirectable( Fn.getName().str() ) )
        {
            continue;
        }

        std::string Signature;
        llvm::raw_string_ostream Printer( Signature );
        Fn.getFunctionType()->print( Printer );
        Out.push_back( { .Name = Fn.getName().str(), .Signature = std::move( Signature ) } );
    }
    return Out;
}
