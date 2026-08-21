// DefineSweep.cpp — the bodies one unit holds.
//
// Symmetric with DeclareAll, and for the same reason: the store is the resolved
// interface of the whole build, so the sweep asks "which of these members does
// *this* unit hold a body for" rather than walking a Decl arena and searching
// the store back. `Member::Unit` is the declaring unit's *discovery* ordinal,
// which is what UnitView::Ordinal carries — deliberately not the view's index,
// since the views are in circuit link order and the two diverge as soon as a
// circuit has edges.

#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"
#include "Functions/ParameterBinder.hpp"
#include "Functions/SignatureBuilder.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

#include <string>
#include <variant>

void Volt::Backend::Llvm::DefineMember ( EmitterServices &Services,
                                         const MiddleEnd::TypeSystem::Member &Entry,
                                         MiddleEnd::TypeSystem::NominalId Owner,
                                         const UnitView &Unit )
{
    // The same four exclusions the declare sweep applies, plus @[External]: that
    // one *has* a symbol — it is declared, and calls to it link — but its body
    // lives outside Volt, so there is nothing here to emit.
    if ( Entry.Kind != MiddleEnd::TypeSystem::EMemberKind::Method or Entry.bAbstract or Entry.OwnGenerics > 0 or
         Entry.ExternSymbol.IsValid() )
    {
        return;
    }

    const auto *Node = std::get_if<Frontend::Method>( &Unit.Ast->Decl( Entry.Decl ) );
    if ( Node == nullptr )
    {
        static_cast<void>(
            Services.Diag->Fail( "llvm: the store calls '" + std::string( Services.Build->Types->Text( Entry.Name ) ) +
                                 "' a method, but its declaration in " + std::string( Unit.Path ) + " is not one" ) );
        return;
    }

    llvm::Function *Fn = Services.Functions->FunctionFor( Entry, Owner, {} );
    if ( Fn == nullptr or not Fn->empty() )
    {
        return;
    }

    MiddleEnd::TypeSystem::TypeStore &Store = *Services.Build->Types;

    // A frame local to this body, so a slot from the previous one cannot resolve
    // a name to storage that no longer exists.
    FunctionFrame Frame;
    Frame.Fn            = Fn;
    Frame.Unit          = &Unit;
    Frame.Values        = Unit.Values;
    Frame.Callees       = Unit.Callees;
    Frame.Owner         = Owner;
    Frame.Entry         = llvm::BasicBlock::Create( Services.Ctx->Context(), "entry", Fn );
    Frame.bReturnsValue = not Fn->getReturnType()->isVoidTy();

    BodyEmitter Emitter( Services, Frame );
    llvm::IRBuilder<> &Builder = Services.Ctx->Builder();
    Builder.SetInsertPoint( Frame.Entry );

    // Parameter order is abi.md's, and it is read here exactly as FunctionTypeOf
    // wrote it — the two are one contract, so `self` leads under the same
    // condition in both.
    unsigned Index = 0;
    if ( Owner.IsValid() and not Entry.bSelf )
    {
        Frame.SelfLayout = Services.Instances->Of( Store, Owner, {} );
        Frame.Self       = Fn->getArg( 0 );
        Frame.Self->setName( "self" );
        Index = 1;
    }

    std::size_t Ordinal = 0;
    for ( const Frontend::ParamId ParamRef : Node->Params )
    {
        llvm::Value *Arg                = Fn->getArg( Index++ );
        const Frontend::Param &Declared = Unit.Ast->GetParam( ParamRef );
        Arg->setName( Unit.Ast->Text( Declared.Name ) );

        // Read out of the *signature*'s own inputs, in the order FunctionTypeOf
        // consumed them, so "by address" cannot drift between the two.
        const bool bBlock = Ordinal < Entry.ParamIsBlock.Size() and Entry.ParamIsBlock[Ordinal];
        const bool bByAddress =
            bBlock or
            ( Ordinal < Entry.Params.Size() and
              Services.Types->IsAggregate( Services.Signatures->SignatureLayoutOf( Store, Entry.Params[Ordinal], Owner, {} ) ) );
        ++Ordinal;

        if ( not BindParameter( Emitter, MiddleEnd::Resolver::BindingSite{ ParamRef }, Arg, bByAddress,
                                Unit.Ast->Text( Declared.Name ) ) )
        {
            static_cast<void>( Emitter.Fail( "llvm: parameter '" + std::string( Unit.Ast->Text( Declared.Name ) ) + "' of '" +
                                             std::string( Store.Text( Entry.Name ) ) + "' has no storage" ) );
            return;
        }

        BindInstanceVarParam( Emitter, ParamRef, Arg );
    }

    Emitter.EmitStmts( Node->Body, Frame.bReturnsValue );

    // Return the type's zero value if control flows off the end of a non-void
    // method without an explicit `return` / tail expression — rules/core-ast.md
    // guarantees Sema checked every path returns, so reaching here means the end
    // of the body is unreachable, but LLVM IR requires every basic block to be
    // terminated anyway.
    if ( not Emitter.Terminated() )
    {
        if ( Frame.bReturnsValue )
        {
            // `getNullValue`, not `ConstantInt::get`: the return type is not
            // always an integer (a `String`-returning method is `{ ptr, i64 }`),
            // and `ConstantInt::get` on a non-integer type is an unchecked `cast`
            // in a release build — silent corruption, not a diagnostic.
            static_cast<void>( Builder.CreateRet( llvm::Constant::getNullValue( Fn->getReturnType() ) ) );
        }
        else
        {
            static_cast<void>( Builder.CreateRetVoid() );
        }
    }
}

void Volt::Backend::Llvm::DefineSynthesizedOnly ( EmitterServices &Services, const UnitView &Unit )
{
    if ( Services.Build == nullptr or Services.Build->Types == nullptr )
    {
        return;
    }
    DeclareSynthesized( Services, Unit );
    DefineSynthesized( Services, Unit );
}

void Volt::Backend::Llvm::DefineAll ( EmitterServices &Services, const UnitView &Unit, bool bInlineEligibleOnly )
{
    if ( Services.Build == nullptr or Services.Build->Types == nullptr )
    {
        return;
    }

    MiddleEnd::TypeSystem::TypeStore &Store = *Services.Build->Types;

    // Declared first, for the same reason DeclareAll runs before any body: a
    // FuncAddr inside an ordinary member's own body (an original closure
    // literal's call site) must find its target already registered.
    DeclareSynthesized( Services, Unit );

    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        const MiddleEnd::TypeSystem::NominalId Id{ static_cast<MiddleEnd::TypeSystem::NominalId::ValueType>( Index ) };
        if ( Store.Type( Id ).Params.Size() > 0 or IsMixinOwner( Services, Id ) )
        {
            continue;
        }

        for ( const MiddleEnd::TypeSystem::Member &Entry : Store.Type( Id ).Members )
        {
            if ( Entry.Unit == Unit.Ordinal )
            {
                if ( bInlineEligibleOnly and Entry.InlineVerdict == MiddleEnd::TypeSystem::EInlineVerdict::Never )
                {
                    continue;
                }
                DefineMember( Services, Entry, Id, Unit );
            }
        }
    }

    for ( const MiddleEnd::TypeSystem::Member &Entry : Store.FreeFunctions() )
    {
        if ( Entry.Unit == Unit.Ordinal )
        {
            if ( bInlineEligibleOnly and Entry.InlineVerdict == MiddleEnd::TypeSystem::EInlineVerdict::Never )
            {
                continue;
            }
            DefineMember( Services, Entry, MiddleEnd::TypeSystem::NominalId{}, Unit );
        }
    }

    DefineSynthesized( Services, Unit );

    if ( not bInlineEligibleOnly )
    {
        EmitUnitInit( Services, Unit );
        EmitUnitFini( Services, Unit );
    }
}
