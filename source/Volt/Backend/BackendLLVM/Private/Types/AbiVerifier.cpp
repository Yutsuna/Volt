// AbiVerifier.cpp — see AbiVerifier.hpp.

#include "Types/AbiVerifier.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>

#include <string>

void Volt::Backend::Llvm::AbiVerifier::VerifyAggregateAbi ( [[maybe_unused]] MiddleEnd::TypeSystem::LayoutId Id,
                                                            [[maybe_unused]] llvm::StructType *Shape )
{
#ifndef NDEBUG
    if ( Shape == nullptr or not Services->Layouts->has_value() or not Services->Ctx->Ready() )
    {
        return;
    }

    const llvm::StructLayout *Native = Services->Ctx->Mod().getDataLayout().getStructLayout( Shape );
    const SizeAlign Ours             = ( *Services->Layouts )->Of( Id );

    if ( Native->getSizeInBytes() != Ours.Size or Native->getAlignment().value() != Ours.Alignment )
    {
        static_cast<void>( Services->Diag->Fail(
            "llvm: ABI divergence on layout " + std::to_string( Id.Value ) + ": LayoutEngine says size " +
            std::to_string( Ours.Size ) + " align " + std::to_string( Ours.Alignment ) + ", LLVM says size " +
            std::to_string( Native->getSizeInBytes() ) + " align " + std::to_string( Native->getAlignment().value() ) ) );
        return;
    }

    for ( unsigned Index = 0; Index < Shape->getNumElements(); ++Index )
    {
        const std::size_t Theirs = Native->getElementOffset( Index );
        const std::size_t Mine   = ( *Services->Layouts )->FieldOffset( Id, Index );
        if ( Theirs != Mine )
        {
            static_cast<void>( Services->Diag->Fail( "llvm: ABI divergence on layout " + std::to_string( Id.Value ) + " field " +
                                                     std::to_string( Index ) + ": LayoutEngine says offset " +
                                                     std::to_string( Mine ) + ", LLVM says " + std::to_string( Theirs ) ) );
            return;
        }
    }
#endif
}
