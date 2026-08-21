// FunctionRegistry.cpp — the symbol cache and on-demand declaration.

#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"
#include "Functions/SignatureBuilder.hpp"

#include "Volt/BackendCore/Mangler.hpp"

#include <iostream>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <string>

std::string Volt::Backend::Llvm::FunctionRegistry::SymbolOf ( const MiddleEnd::TypeSystem::Member &Entry,
                                                              MiddleEnd::TypeSystem::NominalId Owner,
                                                              std::span<const std::uint32_t> FlatArgs ) const
{
    // An @[External] member never enters the mangling scheme: the whole point of
    // that boundary is that the linker and a C compiler agree on the name, so
    // the recorded C spelling is used verbatim.
    if ( Entry.ExternSymbol.IsValid() )
    {
        return std::string( Services->Build->Types->Text( Entry.ExternSymbol ) );
    }
    return MangleFunction( *Services->Build->Types, Entry, Owner, FlatArgs );
}

bool Volt::Backend::Llvm::UnitIsPrecompiled ( const EmitterServices &Services, std::uint32_t UnitOrdinal )
{
    const Ir::IrOptions &Options = *Services.Options;
    if ( UnitOrdinal >= Options.SkipUnitsBelow )
    {
        return false;
    }

    if ( Options.bDefineEntryUnit and Services.Build != nullptr and Services.Build->Types != nullptr and
         not Options.EntryFunction.empty() )
    {
        // Read off the TypeStore rather than matched on a path, so nothing here
        // knows what the prelude is called (rules/zero-hardcode.md).
        const MiddleEnd::TypeSystem::Member *Entry = Services.Build->Types->LookupFunction( Options.EntryFunction );
        if ( Entry != nullptr and Entry->Unit == UnitOrdinal )
        {
            return false;
        }
    }
    return true;
}

llvm::Function *Volt::Backend::Llvm::FunctionRegistry::FunctionFor ( const MiddleEnd::TypeSystem::Member &Entry,
                                                                     MiddleEnd::TypeSystem::NominalId Owner,
                                                                     std::span<const std::uint32_t> FlatArgs )
{
    const std::string Symbol = SymbolOf( Entry, Owner, FlatArgs );
    if ( const auto It = Functions.find( Symbol ); It != Functions.end() )
    {
        return It->second;
    }

    llvm::FunctionType *Signature = Services->Signatures->FunctionTypeOf( Entry, Owner, FlatArgs );
    if ( Signature == nullptr )
    {
        return nullptr;
    }

    const MiddleEnd::TypeSystem::TypeStore &Store = *Services->Build->Types;
    const bool bGeneric        = ( Owner.IsValid() and Store.Type( Owner ).Params.Size() > 0 ) or Entry.OwnGenerics > 0;
    const bool bInlineEligible = ( Entry.InlineVerdict != MiddleEnd::TypeSystem::EInlineVerdict::Never );

    // A body this build will never emit, because the unit it belongs to is
    // already compiled into an artifact we link or load. It has to be a plain
    // external *declaration*: LinkOnceODR describes a definition that may be
    // merged, and a LinkOnceODR global with no body is not valid IR at all —
    // "Global is external, but doesn't have external or weak linkage".
    //
    // Only for the un-instantiated symbol, though. A monomorphised instance
    // (FlatArgs non-empty) is minted per build from the generic's body, so it
    // is not in the artifact and still needs its own mergeable definition here.
    // ... unless this build defines it anyway: the AOT path still emits the
    // inline-eligible bodies below the skip line so an optimised build can
    // inline across the boundary, and a definition must not be a declaration.
    const bool bDefinedHereAnyway = bInlineEligible and Services->Options->bDefineInlineEligibleBelow;
    const bool bDeclaredElsewhere =
        FlatArgs.empty() and not bDefinedHereAnyway and UnitIsPrecompiled( *Services, Entry.Unit );

    const llvm::GlobalValue::LinkageTypes Mergeable =
        Services->Options->bRetainMergeableBodies ? llvm::Function::WeakODRLinkage : llvm::Function::LinkOnceODRLinkage;

    const llvm::GlobalValue::LinkageTypes Linkage =
        ( bDeclaredElsewhere or ( FlatArgs.empty() and not bInlineEligible ) or ( bGeneric and FlatArgs.empty() ) )
            ? llvm::Function::ExternalLinkage
            : Mergeable;
    llvm::Function *Fn = llvm::Function::Create( Signature, Linkage, Symbol, Services->Ctx->ModPtr() );
    switch ( Entry.InlineVerdict )
    {
    case MiddleEnd::TypeSystem::EInlineVerdict::Always:
        Fn->addFnAttr( llvm::Attribute::AlwaysInline );
        break;
    case MiddleEnd::TypeSystem::EInlineVerdict::Hint:
        Fn->addFnAttr( llvm::Attribute::InlineHint );
        break;
    case MiddleEnd::TypeSystem::EInlineVerdict::Never:
        break;
    }
    Functions.emplace( Symbol, Fn );
    return Fn;
}

llvm::Function *Volt::Backend::Llvm::FunctionRegistry::DeclareMember ( const MiddleEnd::TypeSystem::Member &Entry,
                                                                       MiddleEnd::TypeSystem::NominalId Owner )
{
    // A field is storage, not code. An `abstract def` is a contract: either an
    // including type overrides it, or its receiver's layout exempts it and the
    // backend supplies an instruction — no symbol either way.
    if ( Entry.Kind != MiddleEnd::TypeSystem::EMemberKind::Method or Entry.bAbstract )
    {
        return nullptr;
    }

    // A method with its own generics has no signature until a call site fixes
    // them; it is emitted by the monomorphiser, from a MonoRequest.
    if ( Entry.OwnGenerics > 0 )
    {
        return nullptr;
    }

    return FunctionFor( Entry, Owner, {} );
}
