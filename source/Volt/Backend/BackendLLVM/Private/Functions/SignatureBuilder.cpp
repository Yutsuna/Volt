// SignatureBuilder.cpp — see SignatureBuilder.hpp.

#include "Functions/SignatureBuilder.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Types/TypeMapper.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <string>
#include <vector>

Volt::MiddleEnd::TypeSystem::LayoutId
Volt::Backend::Llvm::SignatureBuilder::SignatureLayoutOf ( MiddleEnd::TypeSystem::TypeStore &Store,
                                                           MiddleEnd::TypeSystem::SigTypeId Id,
                                                           MiddleEnd::TypeSystem::NominalId Owner,
                                                           std::span<const std::uint32_t> FlatArgs )
{
    if ( Id.IsValid() and Store.Sig( Id ).ParamIndex == MiddleEnd::TypeSystem::SigType::SelfParam )
    {
        return Services->Instances->Of( Store, Owner, FlatArgs );
    }
    // A nested `self` (`Comparable#..`'s `-> Range<self>`, not a bare `-> self`)
    // needs the receiver's own MonoRequest encoding to substitute into — the
    // same one `Owner`/`FlatArgs` already describe.
    return Services->Instances->OfSignature( Store, Id, FlatArgs, Backend::SelfSubtree( Store, Owner, FlatArgs ) );
}

llvm::FunctionType *Volt::Backend::Llvm::SignatureBuilder::FunctionTypeOf ( const MiddleEnd::TypeSystem::Member &Entry,
                                                                            MiddleEnd::TypeSystem::NominalId Owner,
                                                                            std::span<const std::uint32_t> FlatArgs )
{
    MiddleEnd::TypeSystem::TypeStore &Store = *Services->Build->Types;
    llvm::LLVMContext &Context              = Services->Ctx->Context();
    TypeMapper &Types                       = *Services->Types;

    std::vector<llvm::Type *> Params;
    Params.reserve( Entry.Params.Size() + 1 );

    // `self` leads — except for a static `def self.x`, which has no receiver,
    // and for an @[External] member, whose signature is a C prototype and must
    // carry exactly what the C declaration wrote.
    const bool bExternal = Entry.ExternSymbol.IsValid();
    if ( Owner.IsValid() and not Entry.bSelf and not bExternal )
    {
        llvm::Type *Self = Types.ParamTypeOfLayout( Services->Instances->Of( Store, Owner, FlatArgs ) );
        if ( Self == nullptr )
        {
            static_cast<void>( Services->Diag->Fail( "llvm: receiver of '" + std::string( Store.Text( Entry.Name ) ) +
                                                     "' has no resolved layout" ) );
            return nullptr;
        }
        Params.push_back( Self );
    }

    for ( std::size_t Index = 0; Index < Entry.Params.Size(); ++Index )
    {
        // A `&block` parameter carries a closure, and a closure value is
        // uniformly the two-slot `{ code, env }` aggregate (abi.md) — an
        // aggregate, hence a pointer, whatever the block's own signature says.
        if ( Index < Entry.ParamIsBlock.Size() and Entry.ParamIsBlock[Index] )
        {
            Params.push_back( llvm::PointerType::get( Context, 0 ) );
            continue;
        }

        llvm::Type *Slot = Types.ParamTypeOfLayout( SignatureLayoutOf( Store, Entry.Params[Index], Owner, FlatArgs ) );
        if ( Slot == nullptr )
        {
            static_cast<void>( Services->Diag->Fail( "llvm: parameter " + std::to_string( Index ) + " of '" +
                                                     std::string( Store.Text( Entry.Name ) ) + "' has no resolved layout" ) );
            return nullptr;
        }
        Params.push_back( Slot );
    }

    // No result signature means no return type: a `def` with no `-> T` has none
    // (rules/core-ast.md), and that is the same shape as a declared return whose
    // nominal the stdlib never defines. Both are `void`; neither is guessed at.
    llvm::Type *Result =
        Entry.Result.IsValid() ? Types.TypeOfLayout( SignatureLayoutOf( Store, Entry.Result, Owner, FlatArgs ) ) : nullptr;
    if ( Result == nullptr )
    {
        Result = llvm::Type::getVoidTy( Context );
    }

    return llvm::FunctionType::get( Result, Params, false );
}
