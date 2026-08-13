// ExceptionStorage.cpp — where the in-flight object itself lives, and how wide
// it has to be.
//
// It cannot stay in the raising frame: tier 1 propagates by *returning*, so by
// the time a `rescue` several frames up copies it out — or the last-resort hook
// reports it, with every Volt frame already gone — that alloca is dead storage.
// So `raise` copies the object here first and publishes *this* address.
//
// The size is measured, not guessed: every type descending from the one
// annotated `@[Literal( RaiseExpr )]`, sized by LayoutEngine, the single ABI
// authority. No type name enters.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <cstdint>

namespace
{

// Reflexive and depth-bounded, exactly like the ancestry walk and like Sema's
// own IsSubclassOf: "is `Id` the root, or does its Super chain reach it". The
// bound is the same one TypeStore::LookupMember uses — a malformed cyclic
// hierarchy must not hang codegen.
[[nodiscard]] bool DescendsFrom ( const Volt::MiddleEnd::TypeSystem::TypeStore &Store,
                                  Volt::MiddleEnd::TypeSystem::NominalId Id,
                                  Volt::MiddleEnd::TypeSystem::NominalId Root )
{
    for ( std::uint32_t Depth = 0; Id.IsValid() and Depth <= 16; ++Depth )
    {
        if ( Id == Root )
        {
            return true;
        }
        Id = Store.BaseOf( Store.Type( Id ).Super );
    }
    return false;
}

} // namespace

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
        for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
        {
            const MiddleEnd::TypeSystem::NominalId Id{ static_cast<MiddleEnd::TypeSystem::NominalId::ValueType>( Index ) };
            const MiddleEnd::TypeSystem::LayoutId Shape = Store.Type( Id ).Layout;
            if ( not Shape.IsValid() or not DescendsFrom( Store, Id, *Root ) )
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
