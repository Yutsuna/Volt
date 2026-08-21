#include "Functions/VTableRegistry.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Core/ModuleLocal.hpp"
#include "Functions/FunctionRegistry.hpp"
#include "Types/TypeMapper.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"
#include "Volt/BackendCore/VTableLayout.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <vector>

llvm::GlobalVariable *Volt::Backend::Llvm::VTableRegistry::GetOrCreateVTable ( MiddleEnd::TypeSystem::NominalId Concrete,
                                                                               MiddleEnd::TypeSystem::NominalId Trait )
{
    if ( not Concrete.IsValid() or not Trait.IsValid() or Services == nullptr or Services->Build == nullptr or
         Services->Build->Types == nullptr )
    {
        return nullptr;
    }

    auto &Store = *Services->Build->Types;
    Volt::Backend::VTableEngine Engine{ Store };
    const Volt::Backend::VTableDefinition &Def = Engine.GetDefinition( Concrete, Trait );
    if ( Def.Slots.empty() )
    {
        return nullptr;
    }

    if ( const auto It = Cache.find( Def.SymbolName ); It != Cache.end() )
    {
        return LocalCopy( *Services, It->second );
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Module &Module       = Services->Ctx->Mod();
    llvm::Type *PtrTy          = llvm::PointerType::get( Context, 0 );

    std::vector<llvm::Constant *> Elements;
    Elements.reserve( Def.Slots.size() );

    for ( const auto &Slot : Def.Slots )
    {
        if ( Slot.Decl != nullptr )
        {
            llvm::Function *Fn = Services->Functions->FunctionFor( *Slot.Decl, Concrete, {} );
            if ( Fn != nullptr )
            {
                Elements.push_back( Fn );
            }
            else
            {
                Elements.push_back( llvm::ConstantPointerNull::get( llvm::cast<llvm::PointerType>( PtrTy ) ) );
            }
        }
        else
        {
            Elements.push_back( llvm::ConstantPointerNull::get( llvm::cast<llvm::PointerType>( PtrTy ) ) );
        }
    }

    llvm::ArrayType *ArrayTy = llvm::ArrayType::get( PtrTy, Elements.size() );
    llvm::Constant *Init     = llvm::ConstantArray::get( ArrayTy, Elements );

    // Same reasoning as a unit-scope global's: private to the build while the
    // build is one module, nameable by every other module once it is not.
    const llvm::GlobalValue::LinkageTypes Linkage =
        PerUnitModules( *Services ) ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;

    auto *Global = new llvm::GlobalVariable( Module, ArrayTy, true, Linkage, Init, Def.SymbolName );
    Cache.emplace( Def.SymbolName, Global );
    return Global;
}
