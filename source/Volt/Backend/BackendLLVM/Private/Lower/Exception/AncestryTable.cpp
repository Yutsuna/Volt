// AncestryTable.cpp — the Euler pre-order index for O(1) subtype testing.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <vector>

llvm::GlobalVariable *Volt::Backend::Llvm::ExceptionLowering::PreorderTable ()
{
    if ( PreorderTableVar != nullptr )
    {
        return PreorderTableVar;
    }

    MiddleEnd::TypeSystem::TypeStore &Store = *Services->Build->Types;
    llvm::LLVMContext &Context              = Services->Ctx->Context();
    const std::size_t Count                 = Store.TypeCount();
    llvm::Type *Int32Ty                     = llvm::Type::getInt32Ty( Context );

    std::vector<llvm::Constant *> Rows;
    Rows.reserve( Count );
    for ( std::size_t Index = 0; Index < Count; ++Index )
    {
        const MiddleEnd::TypeSystem::NominalId Id{ static_cast<MiddleEnd::TypeSystem::NominalId::ValueType>( Index ) };
        const auto [Left, Right] = Store.SubtypeInterval( Id );
        Rows.push_back( llvm::ConstantInt::get( Int32Ty, Left ) );
    }

    llvm::ArrayType *TableType = llvm::ArrayType::get( Int32Ty, Count );
    PreorderTableVar = new llvm::GlobalVariable( Services->Ctx->Mod(), TableType, true, llvm::GlobalValue::InternalLinkage,
                                                 llvm::ConstantArray::get( TableType, Rows ), "volt.type.preorder" );
    return PreorderTableVar;
}
