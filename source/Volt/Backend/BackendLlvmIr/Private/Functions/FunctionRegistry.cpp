#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"
#include "Core/ModuleLocal.hpp"
#include "Functions/SignatureBuilder.hpp"

#include "Volt/BackendCore/CompilerSeams.hpp"
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
    if ( Services.PrecompiledUnits == nullptr or UnitOrdinal >= Services.PrecompiledUnits->size() )
    {
        return false;
    }
    return ( *Services.PrecompiledUnits )[UnitOrdinal] != 0;
}

std::vector<std::uint8_t> Volt::Backend::Llvm::ResolvePrecompiledUnits ( const EmitterServices &Services )
{
    const Ir::IrOptions &Options = *Services.Options;

    std::vector<std::uint8_t> Skip( Options.SkipUnitsBelow, static_cast<std::uint8_t>( 1 ) );
    if ( Skip.empty() or Services.Build == nullptr or Services.Build->Types == nullptr )
    {
        return Skip;
    }

    // A unit that declares a compiler seam is emitted here whatever the skip
    // line says, when the artifact is *loaded* rather than linked. The seam's
    // definition is build-specific — `_V_init_all` names this build's units,
    // `_V_symbol_name` this build's symbols — so the artifact's copy answers
    // for the build it was made from. A statically linked build has no such
    // problem: the linker resolves the artifact's reference against this
    // build's definition, and a second copy would be a duplicate symbol.
    if ( not Options.bDefineCompilerSeamUnits )
    {
        return Skip;
    }

    MiddleEnd::TypeSystem::TypeStore &Store = *Services.Build->Types;

    const auto Keep = [&] ( const MiddleEnd::TypeSystem::Member &Entry )
    {
        if ( not Entry.ExternLib.IsValid() or Entry.Unit >= Skip.size() )
        {
            return;
        }
        if ( Store.Text( Entry.ExternLib ) == Backend::CompilerSeams::Library )
        {
            Skip[Entry.Unit] = 0;
        }
    };

    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        const MiddleEnd::TypeSystem::NominalId Id{ static_cast<MiddleEnd::TypeSystem::NominalId::ValueType>( Index ) };
        for ( const MiddleEnd::TypeSystem::Member &Entry : Store.Type( Id ).Members )
        {
            Keep( Entry );
        }
    }
    for ( const MiddleEnd::TypeSystem::Member &Entry : Store.FreeFunctions() )
    {
        Keep( Entry );
    }
    return Skip;
}

llvm::Function *Volt::Backend::Llvm::FunctionRegistry::FunctionFor ( const MiddleEnd::TypeSystem::Member &Entry,
                                                                     MiddleEnd::TypeSystem::NominalId Owner,
                                                                     std::span<const std::uint32_t> FlatArgs )
{
    const std::string Symbol = SymbolOf( Entry, Owner, FlatArgs );
    if ( const auto It = Functions.find( Symbol ); It != Functions.end() )
    {
        // The cache is build-wide and the module is not: under PerUnit
        // granularity the hit may name a function belonging to a module already
        // closed, and what this call site needs is that same function as seen
        // from the one being written.
        return LocalCopy( *Services, It->second );
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
    const bool bDeclaredElsewhere = FlatArgs.empty() and not bDefinedHereAnyway and UnitIsPrecompiled( *Services, Entry.Unit );

    const llvm::GlobalValue::LinkageTypes Mergeable =
        Services->Options->bRetainMergeableBodies ? llvm::Function::WeakODRLinkage : llvm::Function::LinkOnceODRLinkage;

    // Mergeable linkage exists so a linker can fold copies of one body that
    // several translation units each emitted. Splitting a build across modules
    // creates no such copies — a body is emitted in its own unit's module and
    // nowhere else, and an instantiation is deduped before it is emitted at all
    // — while it does create cross-module *references*, which have to be plain
    // external ones. So PerUnit exports everything and merges nothing.
    const llvm::GlobalValue::LinkageTypes Linkage =
        ( PerUnitModules( *Services ) or bDeclaredElsewhere or ( FlatArgs.empty() and not bInlineEligible ) or
          ( bGeneric and FlatArgs.empty() ) )
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

    // Recorded here rather than recomputed at the call site: `bDeclaredElsewhere`
    // is the same question a reload asks — is this body ours to replace — and
    // deriving it twice is how the two answers drift apart.
    if ( not Entry.ExternSymbol.IsValid() and not bDeclaredElsewhere )
    {
        Indirectable.insert( Symbol );
    }
    return Fn;
}

llvm::Function *Volt::Backend::Llvm::FunctionRegistry::Find ( const std::string &Symbol )
{
    const auto It = Functions.find( Symbol );
    return It == Functions.end() ? nullptr : LocalCopy( *Services, It->second );
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
