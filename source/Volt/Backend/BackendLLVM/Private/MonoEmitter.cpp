// MonoEmitter.cpp — drains the Monomorphizer queue.
//
// A generic member has no signature and no body until a call site fixes its
// arguments (LlvmEmitter.cpp's declare/define sweeps skip it outright). A
// call to one, discovered anywhere in EmitResolvedCall, enqueues a
// MonoRequest; this file is what turns each request into a defined
// llvm::Function, exactly the way DefineMember defines a concrete one,
// except the type of every expression and the resolution of every call
// comes from Sema::ReinstantiateBody's overlay rather than from the unit's
// own Values/Callees (Instantiate.hpp: a generic body's ExprIds are shared
// by every instantiation, so the unit's own single-slot-per-ExprId maps
// cannot hold more than one instantiation's answer at a time).
//
// Draining a body can itself discover further instantiations — a generic
// method calling another generic method — so Finalize repeats this until
// the queue is empty rather than draining once.

#include "LlvmState.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/Instantiate.hpp"

#include <llvm/IR/BasicBlock.h>

#include <string>
#include <variant>

const Volt::Sema::Member *Volt::Backend::Llvm::LlvmBackend::State::LookupMonoMember ( const MonoRequest &Request,
                                                                                      const UnitView **OutUnit ) const
{
    const Sema::TypeStore &Store = *Build->Types;

    const Sema::Member *Found = nullptr;
    if ( Request.Owner.IsValid() )
    {
        for ( const Sema::Member &Candidate : Store.Type( Request.Owner ).Members )
        {
            if ( Candidate.Name == Request.Name )
            {
                Found = &Candidate;
                break;
            }
        }
    }
    else
    {
        for ( const Sema::Member &Candidate : Store.FreeFunctions() )
        {
            if ( Candidate.Name == Request.Name )
            {
                Found = &Candidate;
                break;
            }
        }
    }
    if ( Found == nullptr )
    {
        return nullptr;
    }

    for ( const UnitView &View : Build->Units )
    {
        if ( View.Ordinal == Found->Unit )
        {
            *OutUnit = &View;
            return Found;
        }
    }
    return nullptr;
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitMonomorphizedBody ( const MonoRequest &Request )
{
    const UnitView *DeclUnit  = nullptr;
    const Sema::Member *Entry = LookupMonoMember( Request, &DeclUnit );
    if ( Entry == nullptr or DeclUnit == nullptr )
    {
        static_cast<void>( Fail( "llvm: a monomorphisation request names no member this build declares" ) );
        return;
    }

    // The same exclusions DeclareMember/DefineMember apply to a concrete
    // member: a field, an abstract contract, and an @[External] declaration
    // (which has a symbol but no Volt body) are never something this emits.
    if ( Entry->Kind != Sema::EMemberKind::Method or Entry->bAbstract or Entry->ExternSymbol.IsValid() )
    {
        return;
    }

    llvm::Function *Fn = FunctionFor( *Entry, Request.Owner, Request.Args );
    if ( Fn == nullptr or not Fn->empty() )
    {
        // Null means the signature itself failed and Status already names
        // why; non-empty means another request already defined this exact
        // instantiation (FunctionFor's own symbol cache dedupes it).
        return;
    }

    Sema::TypeStore &Store = *Build->Types;

    const auto *MethodNode = std::get_if<Frontend::Method>( &DeclUnit->Ast->Decl( Entry->Decl ) );
    if ( MethodNode == nullptr )
    {
        static_cast<void>( Fail( "llvm: the store calls '" + std::string( Store.Text( Entry->Name ) ) +
                                 "' a method, but its declaration in " + std::string( DeclUnit->Path ) + " is not one" ) );
        return;
    }

    // The one semantic step: re-type this body under the request's concrete
    // bindings. Kept alive for the whole of this function — Frame.Values/
    // Callees below point into it, and nothing here writes back into the
    // unit's own UnitTypes/UnitCallees, ever.
    const Sema::InstantiatedBody Overlay =
        Sema::ReinstantiateBody( Store, *DeclUnit->Ast, *DeclUnit->Scopes, *Entry, Request.Owner, Request.Args );

    Frame               = FunctionFrame{};
    Frame.Fn            = Fn;
    Frame.Unit          = DeclUnit;
    Frame.Values        = &Overlay.Values;
    Frame.Callees       = &Overlay.Callees;
    Frame.Owner         = Request.Owner;
    Frame.OwnerArgs     = Request.Args;
    Frame.Entry         = llvm::BasicBlock::Create( Context, "entry", Fn );
    Frame.bReturnsValue = not Fn->getReturnType()->isVoidTy();
    Builder->SetInsertPoint( Frame.Entry );

    // Parameter order is abi.md's, read here exactly as FunctionTypeOf wrote
    // it — the same contract DefineMember relies on for a concrete member.
    unsigned Index = 0;
    if ( Request.Owner.IsValid() and not Entry->bSelf )
    {
        Frame.SelfLayout = Instances.Of( Store, Request.Owner, Request.Args );
        Frame.Self       = Fn->getArg( 0 );
        Frame.Self->setName( "self" );
        Index = 1;
    }

    for ( const Frontend::ParamId ParamRef : MethodNode->Params )
    {
        if ( Index >= Fn->arg_size() )
        {
            static_cast<void>( Fail( "llvm: '" + std::string( Store.Text( Entry->Name ) ) +
                                     "' declares more parameters in its AST than in its signature" ) );
            return;
        }

        const Frontend::Param &Declared = DeclUnit->Ast->GetParam( ParamRef );
        llvm::Argument *Arg             = Fn->getArg( Index );
        Arg->setName( DeclUnit->Ast->Text( Declared.Name ) );
        ++Index;

        const Sema::BindingSite Site{ ParamRef };
        if ( Arg->getType()->isPointerTy() )
        {
            Frame.Slots.emplace( Site, Arg );
            continue;
        }

        llvm::Value *Slot = SlotFor( Site, Arg->getType(), DeclUnit->Ast->Text( Declared.Name ) );
        if ( Slot == nullptr )
        {
            static_cast<void>( Fail( "llvm: parameter '" + std::string( DeclUnit->Ast->Text( Declared.Name ) ) + "' of '" +
                                     std::string( Store.Text( Entry->Name ) ) + "' has no storage" ) );
            return;
        }
        static_cast<void>( Builder->CreateStore( Arg, Slot ) );
    }

    EmitStmts( MethodNode->Body, Frame.bReturnsValue );

    if ( not Terminated() )
    {
        if ( Frame.bReturnsValue )
        {
            // Falling off the end of a value-returning body — the same
            // honest `unreachable` DefineMember emits for a concrete one
            // (llvm.md: no definite-return analysis).
            static_cast<void>( Builder->CreateUnreachable() );
        }
        else
        {
            static_cast<void>( Builder->CreateRetVoid() );
        }
    }

    Frame = FunctionFrame{};
}

void Volt::Backend::Llvm::LlvmBackend::State::DrainMonomorphizer ()
{
    while ( not Failed() )
    {
        const std::optional<MonoRequest> Request = Mono.Next();
        if ( not Request.has_value() )
        {
            return;
        }
        EmitMonomorphizedBody( *Request );
    }
}
