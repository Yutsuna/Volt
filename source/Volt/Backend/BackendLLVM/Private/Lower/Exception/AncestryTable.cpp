// AncestryTable.cpp — the class hierarchy, emitted once as module data.
//
// One row per declared type: its immediate Super's NominalId, or
// NominalId::InvalidValue for a type with none — the same chain IsSubclassOf
// walks in Sema (TypeResolve.cpp), reused here as *data* rather than as a
// function, since the emitter cannot call back into Sema at runtime.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <vector>

llvm::GlobalVariable *Volt::Backend::Llvm::ExceptionLowering::AncestryTable ()
{
    if ( Ancestry != nullptr )
    {
        return Ancestry;
    }

    Sema::TypeStore &Store     = *Services->Build->Types;
    llvm::LLVMContext &Context = Services->Ctx->Context();
    const std::size_t Count    = Store.TypeCount();
    llvm::Type *Int32Ty        = llvm::Type::getInt32Ty( Context );

    std::vector<llvm::Constant *> Rows;
    Rows.reserve( Count );
    for ( std::size_t Index = 0; Index < Count; ++Index )
    {
        const Sema::NominalId Id{ static_cast<Sema::NominalId::ValueType>( Index ) };
        const Sema::NominalId Super = Store.BaseOf( Store.Type( Id ).Super );
        Rows.push_back( llvm::ConstantInt::get( Int32Ty, Super.IsValid() ? Super.Value : Sema::NominalId::InvalidValue ) );
    }

    llvm::ArrayType *TableType = llvm::ArrayType::get( Int32Ty, Count );
    Ancestry = new llvm::GlobalVariable( Services->Ctx->Mod(), TableType, true, llvm::GlobalValue::InternalLinkage,
                                         llvm::ConstantArray::get( TableType, Rows ), "volt.exc.ancestry" );
    return Ancestry;
}
