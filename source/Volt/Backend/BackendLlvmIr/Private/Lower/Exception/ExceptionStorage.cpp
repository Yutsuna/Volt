// ExceptionStorage.cpp — where the in-flight object itself lives, and how wide
// it has to be.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"
#include "Core/ModuleLocal.hpp"
#include "Volt/BackendCore/UnwindTransport.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <cstdint>

// Measured whether or not a global is created for it: under ETlsAccess::Accessor
// nothing is emitted here, but the JIT still has to be told how wide a buffer to
// hand back, and this is the only place that knows.
void Volt::Backend::Llvm::ExceptionLowering::MeasureStorage ()
{
    if ( ExcStorageSize != 0 )
    {
        return;
    }

    MiddleEnd::TypeSystem::TypeStore &Store = *Services->Build->Types;

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

    ExcStorageSize  = std::max<std::size_t>( Size, 512 );
    ExcStorageAlign = std::max<std::size_t>( Alignment, 8 );
}

llvm::Value *Volt::Backend::Llvm::ExceptionLowering::ExceptionStorageSlot ( llvm::IRBuilderBase &Into )
{
    MeasureStorage();

    if ( Services->Options->Tls == Ir::ETlsAccess::Accessor )
    {
        return SlotThroughAccessor( Into, Volt::Backend::UnwindTransport::SlotTableStorageIndex, "exc.storage" );
    }

    if ( ExcStorage != nullptr )
    {
        return LocalCopy( *Services, ExcStorage );
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::ArrayType *Bytes     = llvm::ArrayType::get( llvm::Type::getInt8Ty( Context ), ExcStorageSize );
    ExcStorage = new llvm::GlobalVariable( Services->Ctx->Mod(), Bytes, false, llvm::GlobalValue::LinkOnceODRLinkage,
                                           llvm::Constant::getNullValue( Bytes ), "volt.exc.storage" );
    ExcStorage->setThreadLocal( true );
    ExcStorage->setAlignment( llvm::Align( ExcStorageAlign ) );
    return ExcStorage;
}
