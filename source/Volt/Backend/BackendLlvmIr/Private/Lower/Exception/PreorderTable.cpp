// PreorderTable.cpp — the Euler pre-order index for O(1) subtype testing.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Core/ModuleLocal.hpp"

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
        return LocalCopy( *Services, PreorderTableVar );
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

    // One table for the build, so the module that first needed it is the one
    // that holds it and the rest declare it. Internal while the build is a
    // single module: nothing outside it has any business reading a table whose
    // indices are this build's own nominal ids.
    const llvm::GlobalValue::LinkageTypes Linkage =
        PerUnitModules( *Services ) ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;

    PreorderTableVar = new llvm::GlobalVariable( Services->Ctx->Mod(), TableType, true, Linkage,
                                                 llvm::ConstantArray::get( TableType, Rows ), "volt.type.preorder" );
    return PreorderTableVar;
}
