// MonoBodyEmitter.cpp — one instantiation's body.
//
// The same shape as DefineMember, with one semantic step in front of it:
// Sema::ReinstantiateBody re-types the body under this request's concrete
// bindings, and the overlay it returns is what the frame reads for every
// expression type and every callee resolution. Nothing here ever writes back
// into the unit's own UnitTypes/UnitCallees — a generic body's ExprIds are
// shared by every instantiation, so those maps cannot hold more than one
// answer at a time.
//
// One deliberate difference from DefineMember, preserved: a value-returning body
// that falls off its end emits `unreachable` here, where a concrete one emits
// `ret zeroinitializer`. See the comment at that site.

#include "Lower/Mono/MonoDriver.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Functions/FunctionRegistry.hpp"
#include "Functions/ParameterBinder.hpp"
#include "Functions/SignatureBuilder.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/Instantiate.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

#include <string>
#include <variant>

void Volt::Backend::Llvm::MonoDriver::EmitMonomorphizedBody ( const MonoRequest &Request )
{
    const UnitView *DeclUnit  = nullptr;
    const Sema::Member *Entry = LookupMonoMember( Request, &DeclUnit );
    if ( Entry == nullptr or DeclUnit == nullptr )
    {
        static_cast<void>( Services->Diag->Fail( "llvm: a monomorphisation request names no member this build declares" ) );
        return;
    }

    // The same exclusions DeclareMember/DefineMember apply to a concrete member:
    // a field, an abstract contract, and an @[External] declaration (which has a
    // symbol but no Volt body) are never something this emits.
    if ( Entry->Kind != Sema::EMemberKind::Method or Entry->bAbstract or Entry->ExternSymbol.IsValid() )
    {
        return;
    }

    llvm::Function *Fn = Services->Functions->FunctionFor( *Entry, Request.Owner, Request.Args );
    if ( Fn == nullptr or not Fn->empty() )
    {
        // Null means the signature itself failed and the diagnostic already names
        // why; non-empty means another request already defined this exact
        // instantiation (FunctionFor's own symbol cache dedupes it).
        return;
    }

    Sema::TypeStore &Store = *Services->Build->Types;

    const auto *MethodNode = std::get_if<Frontend::Method>( &DeclUnit->Ast->Decl( Entry->Decl ) );
    if ( MethodNode == nullptr )
    {
        static_cast<void>( Services->Diag->Fail( "llvm: the store calls '" + std::string( Store.Text( Entry->Name ) ) +
                                                 "' a method, but its declaration in " + std::string( DeclUnit->Path ) +
                                                 " is not one" ) );
        return;
    }

    // Kept alive for the whole of this function — the frame's Values/Callees
    // below point into it.
    const Sema::InstantiatedBody Overlay =
        Sema::ReinstantiateBody( Store, *DeclUnit->Ast, *DeclUnit->Scopes, *Entry, Request.Owner, Request.Args );

    FunctionFrame Frame;
    Frame.Fn            = Fn;
    Frame.Unit          = DeclUnit;
    Frame.Values        = &Overlay.Values;
    Frame.Callees       = &Overlay.Callees;
    Frame.Owner         = Request.Owner;
    Frame.OwnerArgs     = Request.Args;
    Frame.Entry         = llvm::BasicBlock::Create( Services->Ctx->Context(), "entry", Fn );
    Frame.bReturnsValue = not Fn->getReturnType()->isVoidTy();

    BodyEmitter Emitter( *Services, Frame );
    llvm::IRBuilder<> &Builder = Services->Ctx->Builder();
    Builder.SetInsertPoint( Frame.Entry );

    // Parameter order is abi.md's, read here exactly as FunctionTypeOf wrote it —
    // the same contract DefineMember relies on for a concrete member.
    unsigned Index = 0;
    if ( Request.Owner.IsValid() and not Entry->bSelf )
    {
        Frame.SelfLayout = Services->Instances->Of( Store, Request.Owner, Request.Args );
        Frame.Self       = Fn->getArg( 0 );
        Frame.Self->setName( "self" );
        Index = 1;
    }

    std::size_t Ordinal = 0;
    for ( const Frontend::ParamId ParamRef : MethodNode->Params )
    {
        if ( Index >= Fn->arg_size() )
        {
            static_cast<void>( Emitter.Fail( "llvm: '" + std::string( Store.Text( Entry->Name ) ) +
                                             "' declares more parameters in its AST than in its signature" ) );
            return;
        }

        const Frontend::Param &Declared = DeclUnit->Ast->GetParam( ParamRef );
        llvm::Argument *Arg             = Fn->getArg( Index );
        Arg->setName( DeclUnit->Ast->Text( Declared.Name ) );
        ++Index;

        // Same reading as DefineMember's, at this request's own arguments: the
        // signature decided by address or by value from the layout, and so does
        // the binding.
        const bool bBlock = Ordinal < Entry->ParamIsBlock.Size() and Entry->ParamIsBlock[Ordinal];
        const bool bByAddress =
            bBlock or ( Ordinal < Entry->Params.Size() and
                        Services->Types->IsAggregate( Services->Signatures->SignatureLayoutOf(
                            Store, Entry->Params[Ordinal], Request.Owner, Request.Args ) ) );
        ++Ordinal;

        if ( not BindParameter( Emitter, Sema::BindingSite{ ParamRef }, Arg, bByAddress,
                                DeclUnit->Ast->Text( Declared.Name ) ) )
        {
            static_cast<void>( Emitter.Fail( "llvm: parameter '" + std::string( DeclUnit->Ast->Text( Declared.Name ) ) +
                                             "' of '" + std::string( Store.Text( Entry->Name ) ) + "' has no storage" ) );
            return;
        }
        BindInstanceVarParam( Emitter, ParamRef, Arg );
    }

    Emitter.EmitStmts( MethodNode->Body, Frame.bReturnsValue );

    if ( not Emitter.Terminated() )
    {
        if ( Frame.bReturnsValue )
        {
            // Falling off the end of a value-returning body — the honest
            // `unreachable` (llvm.md: no definite-return analysis). Deliberately
            // *not* the same as DefineMember's `ret zeroinitializer`: a concrete
            // body reaching its end is a shape LLVM still has to terminate,
            // whereas an instantiation reaching it means the request itself was
            // ill-formed, and `unreachable` is what says so.
            static_cast<void>( Builder.CreateUnreachable() );
        }
        else
        {
            static_cast<void>( Builder.CreateRetVoid() );
        }
    }
}
