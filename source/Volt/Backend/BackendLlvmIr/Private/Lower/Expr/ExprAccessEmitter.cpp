// ExprAccessEmitter.cpp — reading a name: `x`, `@x`, `o.x`, `self`, `super`,
// and the address of an already-resolved function.
//
// The thing all of these share is that a bare name is *ambiguous by design* in
// Volt: `test_int8` and `symbols.free` are paren-less calls, spelled exactly
// like a local read and a field read. Only the resolution Sema recorded tells
// them apart (rules/core-ast.md), so that decision is read here, never
// re-derived.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/EmitterServices.hpp"
#include "Functions/FunctionRegistry.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include <llvm/IR/Function.h>

#include <string>

std::string Volt::Backend::Llvm::BodyEmitter::MissingSelf ( std::string_view What ) const
{
    if ( Frame().bClosure )
    {
        return "llvm: " + std::string( What ) +
               " inside a closure body — SynthesizeClosureFrame captures bindings, and a receiver is not one, so nothing "
               "carries `self` into the environment";
    }
    return "llvm: " + std::string( What ) + " outside a method with a receiver";
}

llvm::Value *Volt::Backend::Llvm::EmitIdentifierValue ( BodyEmitter &Emitter, Frontend::ExprId Id )
{
    // `test_int8` — a paren-less, receiver-less call is a bare `Identifier`,
    // exactly as a paren-less `Member` is. EmitResolvedCall already handles a
    // receiver-less entry (`Receiver` invalid, `Frame.Self` or no owner at all
    // for a module-level free function).
    if ( const MiddleEnd::IR::CalleeEntry *Entry = Emitter.Frame().Callees->Get( Id );
         Entry != nullptr and Entry->Decl != nullptr and Entry->Decl->Kind == MiddleEnd::TypeSystem::EMemberKind::Method )
    {
        return Emitter.EmitResolvedCall( Id, *Entry, Frontend::ExprId{}, {}, Frontend::ExprId{} );
    }
    return Emitter.LoadPlace( Id );
}

llvm::Value *Volt::Backend::Llvm::EmitMemberValue ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::Member &Node )
{
    // `symbols.free` — a paren-less call is a bare `Member`, and only the
    // resolution tells it apart from a field read.
    if ( const MiddleEnd::IR::CalleeEntry *Entry = Emitter.Frame().Callees->Get( Id );
         Entry != nullptr and Entry->Decl != nullptr and Entry->Decl->Kind == MiddleEnd::TypeSystem::EMemberKind::Method )
    {
        return Emitter.EmitResolvedCall( Id, *Entry, Node.Object, {}, Frontend::ExprId{} );
    }
    return Emitter.LoadPlace( Id );
}

llvm::Value *Volt::Backend::Llvm::EmitSelf ( BodyEmitter &Emitter )
{
    if ( Emitter.Frame().Self == nullptr )
    {
        static_cast<void>( Emitter.Fail( Emitter.MissingSelf( "`self`" ) ) );
    }
    return Emitter.Frame().Self;
}

llvm::Value *Volt::Backend::Llvm::EmitSuper ( BodyEmitter &Emitter )
{
    if ( Emitter.Frame().Self == nullptr )
    {
        static_cast<void>( Emitter.Fail( Emitter.MissingSelf( "`super`" ) ) );
    }
    return Emitter.Frame().Self;
}

llvm::Value *Volt::Backend::Llvm::EmitFuncAddr ( BodyEmitter &Emitter, const Frontend::FuncAddr &Node )
{
    // Target names an already-resolved Method Decl (core-ast.md's "Inert"
    // category — nothing to descend into). Opaque pointers mean a llvm::Function*
    // used as a value already has type `ptr`, so once found it is returned
    // directly, no cast.
    //
    // Two sources, tried in order: an ordinary top-level `def`, registered in the
    // cross-unit TypeStore like any other free function; failing that, a function
    // ClosureLifting synthesized for this unit alone (MiddleEnd::IR::SynthesizedFunctions
    // — never a TypeStore member), keyed the same way DeclareSynthesizedFn
    // registered it.
    const FunctionFrame &Frame = Emitter.Frame();
    EmitterServices &Services  = Emitter.Services();

    if ( const MiddleEnd::TypeSystem::Member *Entry = Services.Build->Types->FunctionByDecl( Frame.Unit->Ordinal, Node.Target );
         Entry != nullptr )
    {
        return Services.Functions->FunctionFor( *Entry, MiddleEnd::TypeSystem::NominalId{}, {} );
    }

    const auto It = Services.SynthesizedFns->find( UnitDeclKey{ .Ordinal = Frame.Unit->Ordinal, .Decl = Node.Target } );
    if ( It == Services.SynthesizedFns->end() )
    {
        static_cast<void>( Emitter.Fail( "llvm: FuncAddr targets a Decl with no resolved free-function entry" ) );
        return nullptr;
    }
    return It->second;
}
