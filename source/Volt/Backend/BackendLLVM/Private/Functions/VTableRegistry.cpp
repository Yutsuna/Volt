#include "Functions/VTableRegistry.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Functions/FunctionRegistry.hpp"
#include "Types/TypeMapper.hpp"
#include "Volt/BackendCore/Mangler.hpp"

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

    auto &Store                       = *Services->Build->Types;
    const std::string ConcreteMangled = Volt::Backend::MangleNominal( Store, Concrete );
    const std::string TraitMangled    = Volt::Backend::MangleNominal( Store, Trait );
    const std::string VTableName      = "_VTable_" + ConcreteMangled + "_" + TraitMangled;

    if ( const auto It = Cache.find( VTableName ); It != Cache.end() )
    {
        return It->second;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Module &Module       = Services->Ctx->Mod();
    llvm::Type *PtrTy          = llvm::PointerType::get( Context, 0 );

    const auto Slots = Store.VTableSlotsOf( Trait );
    std::vector<llvm::Constant *> Elements;
    Elements.reserve( 1 + Slots.size() );

    // Slot 0: Finalize (drop_in_place)
    const bool bTrivial = Store.Type( Concrete ).bTrivialFinalize;
    if ( bTrivial )
    {
        Elements.push_back( llvm::ConstantPointerNull::get( llvm::cast<llvm::PointerType>( PtrTy ) ) );
    }
    else
    {
        const auto *FinDecl = Store.OwnMember( Concrete, "finalize" );
        if ( FinDecl != nullptr )
        {
            llvm::Function *FinFn = Services->Functions->FunctionFor( *FinDecl, Concrete, {} );
            if ( FinFn != nullptr )
            {
                Elements.push_back( FinFn );
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

    // Slots 1..N: Virtual methods in declaration order
    for ( const auto MethodSym : Slots )
    {
        const std::string_view MethodName = Store.Text( MethodSym );
        const auto Found                  = Store.LookupMember( Concrete, MethodName );
        if ( Found.Decl != nullptr )
        {
            llvm::Function *Fn = Services->Functions->FunctionFor( *Found.Decl, Concrete, {} );
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

    auto *Global = new llvm::GlobalVariable( Module, ArrayTy, true, llvm::GlobalValue::InternalLinkage, Init, VTableName );
    Cache.emplace( VTableName, Global );
    return Global;
}
