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

    // A consumer that emits repeatedly into one live program may already hold
    // this array, and if it does, this emission must read *its* copy rather
    // than build a second one: the pointers in a vtable are storage, and a hot
    // reload repoints them by writing into that storage. A second copy would
    // be a second answer to the same dispatch, and only one of the two would
    // ever be patched.
    const bool bElsewhere = Services->Options != nullptr and Services->Options->IsAlreadyDefined and
                            Services->Options->IsAlreadyDefined( Def.SymbolName );

    // Constant is what a vtable *is* — and the only reason to give that up is
    // that a reload has to write into it, which is exactly what indirect
    // linkage announces. An ahead-of-time build keeps the read-only placement
    // and the devirtualisation that comes with it.
    const bool bMovable = IndirectLinkage( *Services );

    std::vector<llvm::Constant *> Elements;
    Elements.reserve( Def.Slots.size() );

    for ( const auto &Slot : Def.Slots )
    {
        llvm::Function *Fn = Slot.Decl != nullptr ? Services->Functions->FunctionFor( *Slot.Decl, Concrete, {} ) : nullptr;
        if ( Fn == nullptr )
        {
            Elements.push_back( llvm::ConstantPointerNull::get( llvm::cast<llvm::PointerType>( PtrTy ) ) );
            continue;
        }

        if ( bMovable )
        {
            Recorded.push_back( { .VTable   = Def.SymbolName,
                                  .Slot     = static_cast<std::uint32_t>( Elements.size() ),
                                  .Function = Fn->getName().str() } );
        }
        Elements.push_back( Fn );
    }

    llvm::ArrayType *ArrayTy = llvm::ArrayType::get( PtrTy, Elements.size() );

    // Same reasoning as a unit-scope global's: private to the build while the
    // build is one module, nameable by every other module once it is not.
    const llvm::GlobalValue::LinkageTypes Linkage =
        PerUnitModules( *Services ) ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;

    llvm::Constant *Init = bElsewhere ? nullptr : llvm::ConstantArray::get( ArrayTy, Elements );

    auto *Global = new llvm::GlobalVariable( Module, ArrayTy, not bMovable, Linkage, Init, Def.SymbolName );
    Cache.emplace( Def.SymbolName, Global );
    return Global;
}
