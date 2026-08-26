// DeclareSweep.cpp — every symbol the build can name, before any body exists.
//
// The TypeStore is the sweep's input, not the ASTs: it is the build-wide,
// already-resolved interface of every unit, so one pass over it covers all units
// at once and a DeclId — meaningful only inside the arena that minted it — never
// has to leave its unit. Declaring everything up front is what lets a body
// emitted in the first unit call something declared in the last with no fixup
// pass.

#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"

#include "Volt/BackendCore/InitAllSynthesizer.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <string>
#include <variant>

bool Volt::Backend::Llvm::IsMixinOwner ( const EmitterServices &Services, MiddleEnd::TypeSystem::NominalId Id )
{
    if ( not Id.IsValid() or Services.Build == nullptr )
    {
        return false;
    }

    const MiddleEnd::TypeSystem::TypeStore &Store  = *Services.Build->Types;
    const MiddleEnd::TypeSystem::NominalType &Type = Store.Type( Id );
    for ( const UnitView &View : Services.Build->Units )
    {
        if ( View.Ordinal == Type.Unit and View.Ast != nullptr )
        {
            return std::holds_alternative<Frontend::Mixin>( View.Ast->Decl( Type.Decl ) );
        }
    }
    return false;
}

void Volt::Backend::Llvm::DeclareAll ( EmitterServices &Services )
{
    if ( Services.Build == nullptr or Services.Build->Types == nullptr )
    {
        return;
    }

    MiddleEnd::TypeSystem::TypeStore &Store = *Services.Build->Types;

    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        const MiddleEnd::TypeSystem::NominalId Id{ static_cast<MiddleEnd::TypeSystem::NominalId::ValueType>( Index ) };

        // A generic type's members have no shape until its arguments are fixed;
        // `Array<Int32>#push` is minted by the monomorphiser instead. A mixin's
        // own concrete methods are generic over `self` in exactly the same sense,
        // and no less so for declaring zero `<T>`s of its own — IsMixinOwner is
        // the check that catches it.
        if ( Store.Type( Id ).Params.Size() > 0 or IsMixinOwner( Services, Id ) )
        {
            continue;
        }

        for ( const MiddleEnd::TypeSystem::Member &Entry : Store.Type( Id ).Members )
        {
            static_cast<void>( Services.Functions->DeclareMember( Entry, Id ) );
        }
    }

    for ( const MiddleEnd::TypeSystem::Member &Entry : Store.FreeFunctions() )
    {
        static_cast<void>( Services.Functions->DeclareMember( Entry, MiddleEnd::TypeSystem::NominalId{} ) );
    }

    // Every unit's initialiser, for the one function that names them all.
    //
    // Guarded on the same condition `_V_init_all` itself is: EmitEntryPoint
    // returns before EmitInitAll when there is no entry symbol, so an emission
    // without one never calls a unit init it did not define. Declaring them
    // anyway is not merely waste — it is waste proportional to the number of
    // units, paid on *every* emission. For a consumer that emits once that is
    // invisible; for one that emits repeatedly into a growing build it is the
    // difference between a flat cost per emission and a quadratic one.
    if ( Services.Options->EntrySymbol.empty() )
    {
        return;
    }

    llvm::FunctionType *InitFnTy = llvm::FunctionType::get( Services.Ctx->Builder().getVoidTy(), false );
    for ( const UnitView &Unit : Services.Build->Units )
    {
        // Same rule EmitInitAll names them by: a unit with an empty top level
        // has no initializer to call, so declaring one would be a reference to
        // a symbol nothing defines.
        if ( not Backend::UnitHasInit( Unit ) )
        {
            continue;
        }
        const std::string InitName = "_V_init_" + std::to_string( Unit.Ordinal );
        if ( Services.Ctx->Mod().getFunction( InitName ) == nullptr )
        {
            llvm::Function::Create( InitFnTy, llvm::Function::ExternalLinkage, InitName, Services.Ctx->ModPtr() );
        }
    }
}
