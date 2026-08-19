// ExceptionStorage.cpp — where the in-flight object itself lives, and how wide
// it has to be.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <cstdint>

llvm::GlobalVariable *Volt::Backend::Llvm::ExceptionLowering::ExceptionStorageSlot ()
{
    if ( ExcStorage != nullptr )
    {
        return ExcStorage;
    }

    MiddleEnd::TypeSystem::TypeStore &Store = *Services->Build->Types;
    llvm::LLVMContext &Context              = Services->Ctx->Context();

    // An empty answer (no root declared, so nothing can be raised at all) still
    // yields a valid one-byte buffer rather than a zero-length global.
    std::size_t Size      = 1;
    std::size_t Alignment = 1;
    if ( const auto Root = Store.LookupNodeKind( "RaiseExpr" ); Root.has_value() and Services->Layouts->has_value() )
    {
        const auto [RootLeft, RootRight] = Store.SubtypeInterval( *Root );
        for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
        {
            const MiddleEnd::TypeSystem::NominalId Id{ static_cast<MiddleEnd::TypeSystem::NominalId::ValueType>( Index ) };
            const auto [Left, Right] = Store.SubtypeInterval( Id );
            if ( Left < RootLeft or Left > RootRight )
            {
                continue; // O(1) subtype check
            }
            const MiddleEnd::TypeSystem::LayoutId Shape = Store.Type( Id ).Layout;
            if ( not Shape.IsValid() )
            {
                continue;
            }
            const SizeAlign Measured = ( *Services->Layouts )->Of( Shape );
            Size                     = std::max( Size, Measured.Size );
            Alignment                = std::max( Alignment, Measured.Alignment );
        }
    }

    Size                   = std::max<std::size_t>( Size, 512 );
    Alignment              = std::max<std::size_t>( Alignment, 8 );
    llvm::ArrayType *Bytes = llvm::ArrayType::get( llvm::Type::getInt8Ty( Context ), Size );
    ExcStorage             = new llvm::GlobalVariable( Services->Ctx->Mod(), Bytes, false, llvm::GlobalValue::LinkOnceODRLinkage,
                                                       llvm::Constant::getNullValue( Bytes ), "volt.exc.storage" );
    ExcStorage->setThreadLocal( true );
    ExcStorage->setAlignment( llvm::Align( Alignment ) );
    ExcStorageSize = Size;
    return ExcStorage;
}
