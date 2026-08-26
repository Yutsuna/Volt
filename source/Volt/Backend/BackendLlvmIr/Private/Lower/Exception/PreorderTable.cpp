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

#include <string>
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
    const bool bPerUnit = PerUnitModules( *Services );
    const llvm::GlobalValue::LinkageTypes Linkage =
        bPerUnit ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;

    // Named for the corpus it describes, not for the build. A session that
    // grows — `volt repl`, where every line is an emission of its own and each
    // one starts with an ExceptionLowering that has never seen a table — cannot
    // share one name across lines: the second line would define
    // `volt.type.preorder` a second time and the JIT rejects the whole module,
    // taking every `def` on that line down with it. It cannot share one *table*
    // either, because a class declared at the prompt makes the old one too
    // short for the bound check EmitAncestorTest emits against the current
    // TypeCount. Both are the same fact — the table is a function of the type
    // count — so the count is in the name: lines that agree on it share a
    // definition, and a line that grew the corpus gets a table sized for it.
    const std::string Name = "volt.type.preorder." + std::to_string( Count );

    if ( bPerUnit and Services->Options->IsAlreadyDefined and Services->Options->IsAlreadyDefined( Name ) )
    {
        // Already defined by an earlier emission that is resident in the same
        // process. No initialiser: that is what makes this a declaration
        // rather than a second definition, exactly as LocalCopy does it.
        PreorderTableVar =
            new llvm::GlobalVariable( Services->Ctx->Mod(), TableType, true, llvm::GlobalValue::ExternalLinkage, nullptr, Name );
        return PreorderTableVar;
    }

    PreorderTableVar = new llvm::GlobalVariable( Services->Ctx->Mod(), TableType, true, Linkage,
                                                 llvm::ConstantArray::get( TableType, Rows ), Name );
    return PreorderTableVar;
}
