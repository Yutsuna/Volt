// FunctionRegistry.cpp — the symbol cache and on-demand declaration.

#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"
#include "Functions/SignatureBuilder.hpp"

#include "Volt/BackendCore/Mangler.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <string>

std::string Volt::Backend::Llvm::FunctionRegistry::SymbolOf ( const Sema::Member &Entry,
                                                              Sema::NominalId Owner,
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

llvm::Function *Volt::Backend::Llvm::FunctionRegistry::FunctionFor ( const Sema::Member &Entry,
                                                                     Sema::NominalId Owner,
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

    // A monomorphised instantiation (FlatArgs non-empty — only
    // EmitMonomorphizedBody ever calls FunctionFor that way; every concrete
    // declare/define call site passes {}) is `linkonce_odr`, not `External`:
    // `Array<UInt8>` used internally by a precompiled stdlib archive and
    // independently instantiated by a user build mangle to the *same* symbol
    // (Mangler.hpp is deterministic and content-only), and without weak linkage
    // the two definitions collide at link time (issue #61 blind-spot #3).
    // `linkonce_odr` tells the linker any one definition will do, which is
    // exactly true — Monomorphizer only ever reinstantiates the same body for
    // the same FlatArgs.
    const llvm::GlobalValue::LinkageTypes Linkage =
        FlatArgs.empty() ? llvm::Function::ExternalLinkage : llvm::Function::LinkOnceODRLinkage;
    llvm::Function *Fn = llvm::Function::Create( Signature, Linkage, Symbol, Services->Ctx->ModPtr() );
    Functions.emplace( Symbol, Fn );
    return Fn;
}

llvm::Function *Volt::Backend::Llvm::FunctionRegistry::DeclareMember ( const Sema::Member &Entry, Sema::NominalId Owner )
{
    // A field is storage, not code. An `abstract def` is a contract: either an
    // including type overrides it, or its receiver's layout exempts it and the
    // backend supplies an instruction — no symbol either way.
    if ( Entry.Kind != Sema::EMemberKind::Method or Entry.bAbstract )
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
