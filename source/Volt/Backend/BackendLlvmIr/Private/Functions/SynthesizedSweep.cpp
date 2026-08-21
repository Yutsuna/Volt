// SynthesizedSweep.cpp — the functions ClosureLifting produced.
//
// A synthesized function has no self, no mangling and no cross-unit symbol: it
// is reached only through a FuncAddr naming its Decl directly, so its LLVM name
// only has to be distinct within the module, never resolvable. Its signature
// comes straight from already-resolved SemaTypeIds — the lowering pass that
// created the entry only ever runs once those have settled — instead of a
// MiddleEnd::TypeSystem::Member's SigTypeId/FlatArgs substitution.
//
// Declaration and definition are two sweeps rather than one for a reason stated
// at DeclareSynthesized's declaration: a FuncAddr in an *ordinary* member's body
// must find its target registered regardless of emission order.

#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"
#include "Functions/ParameterBinder.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <string>
#include <variant>
#include <vector>

void Volt::Backend::Llvm::DeclareSynthesizedFn ( EmitterServices &Services,
                                                 const MiddleEnd::IR::SynthesizedFunction &Fn,
                                                 const UnitView &Unit,
                                                 const MiddleEnd::TypeSystem::UnitTypes &Values )
{
    llvm::LLVMContext &Context = Services.Ctx->Context();

    std::vector<llvm::Type *> Params;
    Params.reserve( Fn.Params.Size() );
    for ( const MiddleEnd::TypeSystem::SemaTypeId Param : Fn.Params )
    {
        llvm::Type *Slot = Services.Types->ParamTypeOfLayout( Services.Types->LayoutOfValue( Values, Param ) );
        if ( Slot == nullptr )
        {
            static_cast<void>( Services.Diag->Fail( "llvm: a synthesized function's parameter in " + std::string( Unit.Path ) +
                                                    " has no resolved layout" ) );
            return;
        }
        Params.push_back( Slot );
    }

    llvm::Type *Result =
        Fn.Result.IsValid() ? Services.Types->TypeOfLayout( Services.Types->LayoutOfValue( Values, Fn.Result ) ) : nullptr;
    if ( Result == nullptr )
    {
        Result = llvm::Type::getVoidTy( Context );
    }

    llvm::FunctionType *Signature = llvm::FunctionType::get( Result, Params, false );
    const std::string Name        = "__synth." + std::to_string( Unit.Ordinal ) + "." + std::to_string( Fn.Decl.Value );
    llvm::Function *LlvmFn = llvm::Function::Create( Signature, llvm::Function::PrivateLinkage, Name, Services.Ctx->ModPtr() );
    Services.SynthesizedFns->emplace( UnitDeclKey{ .Ordinal = Unit.Ordinal, .Decl = Fn.Decl }, LlvmFn );
}

void Volt::Backend::Llvm::DeclareSynthesized ( EmitterServices &Services, const UnitView &Unit )
{
    if ( Unit.Synth == nullptr or Unit.Values == nullptr )
    {
        return;
    }
    for ( const MiddleEnd::IR::SynthesizedFunction &Fn : Unit.Synth->All() )
    {
        DeclareSynthesizedFn( Services, Fn, Unit, *Unit.Values );
    }
}

void Volt::Backend::Llvm::DefineSynthesizedFn ( EmitterServices &Services,
                                                const MiddleEnd::IR::SynthesizedFunction &Fn,
                                                const UnitView &Unit,
                                                const MiddleEnd::TypeSystem::UnitTypes &Values,
                                                const MiddleEnd::IR::UnitCallees &Callees,
                                                const MiddleEnd::IR::ExprRedirectMap *Redirects )
{
    const auto *Node = std::get_if<Frontend::Method>( &Unit.Ast->Decl( Fn.Decl ) );
    if ( Node == nullptr )
    {
        static_cast<void>( Services.Diag->Fail( "llvm: a synthesized function in " + std::string( Unit.Path ) +
                                                " has no Method declaration behind it" ) );
        return;
    }

    const auto It = Services.SynthesizedFns->find( UnitDeclKey{ .Ordinal = Unit.Ordinal, .Decl = Fn.Decl } );
    if ( It == Services.SynthesizedFns->end() )
    {
        // DeclareSynthesized already reported why this entry has no
        // llvm::Function.
        return;
    }
    llvm::Function *LlvmFn = It->second;
    llvm::Type *Result     = LlvmFn->getReturnType();

    FunctionFrame Frame;
    Frame.Fn            = LlvmFn;
    Frame.Unit          = &Unit;
    Frame.Values        = &Values;
    Frame.Callees       = &Callees;
    Frame.Redirects     = Redirects;
    Frame.Entry         = llvm::BasicBlock::Create( Services.Ctx->Context(), "entry", LlvmFn );
    Frame.bReturnsValue = not Result->isVoidTy();
    // Every SynthesizedFunction is a lifted Lambda/Block (ClosureLifting is its
    // only producer), so a bare `break`/`next` in its body is a non-local exit;
    // bClosure routes the statement emitter's Break/Next arms onto the
    // unwind-sentinel transport (BreakFlagSlot / EmitBlockNext) instead of
    // failing with "outside a loop reached codegen".
    Frame.bClosure = true;

    BodyEmitter Emitter( Services, Frame );
    llvm::IRBuilder<> &Builder = Services.Ctx->Builder();
    Builder.SetInsertPoint( Frame.Entry );

    unsigned Index = 0;
    for ( const Frontend::ParamId ParamRef : Node->Params )
    {
        llvm::Value *Arg                = LlvmFn->getArg( Index++ );
        const Frontend::Param &Declared = Unit.Ast->GetParam( ParamRef );
        Arg->setName( Unit.Ast->Text( Declared.Name ) );

        const MiddleEnd::Resolver::BindingSite Site{ ParamRef };
        const bool bByAddress = Services.Types->IsAggregate( Services.Types->LayoutOfValue( Values, Values.SiteType( Site ) ) );
        if ( not BindParameter( Emitter, Site, Arg, bByAddress, Unit.Ast->Text( Declared.Name ) ) )
        {
            static_cast<void>( Emitter.Fail( "llvm: a synthesized function's parameter '" +
                                             std::string( Unit.Ast->Text( Declared.Name ) ) + "' has no storage" ) );
            return;
        }
    }

    Emitter.EmitStmts( Node->Body, Frame.bReturnsValue );

    if ( not Emitter.Terminated() )
    {
        if ( Frame.bReturnsValue )
        {
            static_cast<void>( Builder.CreateRet( llvm::Constant::getNullValue( Result ) ) );
        }
        else
        {
            static_cast<void>( Builder.CreateRetVoid() );
        }
    }
}

void Volt::Backend::Llvm::DefineSynthesized ( EmitterServices &Services, const UnitView &Unit )
{
    if ( Unit.Synth == nullptr or Unit.Values == nullptr or Unit.Callees == nullptr )
    {
        return;
    }
    for ( const MiddleEnd::IR::SynthesizedFunction &Fn : Unit.Synth->All() )
    {
        DefineSynthesizedFn( Services, Fn, Unit, *Unit.Values, *Unit.Callees );
    }
}
