// EntryPointEmitter.cpp — `_V_init_all`, and the C shim the runtime starts at.
//
// Both are synthesised rather than emitted from a Volt body, which is why they
// are hand-rolled here instead of going through BodyEmitter. What they
// deliberately do *not* do is decide anything: reporting an uncaught exception
// and choosing the exit status are `__volt_entry`'s own `begin/rescue` in
// source/Lib/Prelude.vl (rules/zero-hardcode.md). No field name, no type name,
// no byte of message enters C++ here.

#include "Functions/FunctionRegistry.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/Exception/ExceptionLowering.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"
#include "Volt/BackendCore/InitAllSynthesizer.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <string>

bool Volt::Backend::Llvm::EmitInitAll ( EmitterServices &Services )
{
    // `_V_init_all`: the one symbol the stdlib prelude's
    // `@[External( "volt", "_V_init_all" )]` declaration names. DeclareAll has
    // already created it as an external declaration — the same shape every other
    // @[External] member gets, "declared, never defined" — this gives that
    // declaration its one and only body, called exactly once, by the prelude's
    // `__volt_entry`. A raise inside it is carried out through the ordinary
    // post-call check every Volt call site already gets; this function's own job
    // is only to stop running *further* unit inits once one has left the tag set.
    llvm::LLVMContext &Context = Services.Ctx->Context();
    llvm::Module &Mod          = Services.Ctx->Mod();

    llvm::Function *InitAllFn = Mod.getFunction( "_V_init_all" );
    if ( InitAllFn == nullptr )
    {
        llvm::FunctionType *FnTy = llvm::FunctionType::get( Services.Ctx->Builder().getVoidTy(), false );
        InitAllFn                = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, "_V_init_all", &Mod );
    }
    if ( not InitAllFn->empty() )
    {
        return true;
    }

    llvm::Type *Int32Ty = llvm::Type::getInt32Ty( Context );
    llvm::IRBuilder<> Shell{ llvm::BasicBlock::Create( Context, "entry", InitAllFn ) };

    if ( Services.Build != nullptr )
    {
        const auto Steps = Backend::SynthesizeInitAll( Services.Build->Units );
        for ( const auto &Step : Steps )
        {
            llvm::Function *InitFn = Mod.getFunction( Step.Symbol );
            if ( InitFn == nullptr )
            {
                llvm::FunctionType *FnTy = llvm::FunctionType::get( Services.Ctx->Builder().getVoidTy(), false );
                InitFn                   = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, Step.Symbol, &Mod );
            }
            static_cast<void>( Shell.CreateCall( InitFn ) );

            if ( not Step.bLast )
            {
                llvm::Value *Tag     = Shell.CreateLoad( Int32Ty, Services.Exceptions->ExceptionTagSlot( Shell ), "exc.tag" );
                llvm::Value *Pending = Shell.CreateICmpNE(
                    Tag, llvm::ConstantInt::get( Int32Ty, MiddleEnd::TypeSystem::NominalId::InvalidValue ), "exc.pending" );

                llvm::BasicBlock *Stop = llvm::BasicBlock::Create( Context, "init.stop", InitAllFn );
                llvm::BasicBlock *Next = llvm::BasicBlock::Create( Context, "init.next", InitAllFn );
                static_cast<void>( Shell.CreateCondBr( Pending, Stop, Next ) );

                Shell.SetInsertPoint( Stop );
                static_cast<void>( Shell.CreateRetVoid() );

                Shell.SetInsertPoint( Next );
            }
        }
    }

    static_cast<void>( Shell.CreateRetVoid() );
    return true;
}

bool Volt::Backend::Llvm::EmitFiniAll ( EmitterServices &Services )
{
    // `_V_fini_all`: the mirror of `_V_init_all`, and the one symbol the
    // prelude's `@[External( "volt", "_V_fini_all" )]` declaration names.
    //
    // No post-call check between steps, unlike the init sequence. Teardown runs
    // *because* the program is over — including when it is over on account of
    // an exception — so an exception in flight is not a reason to stop
    // releasing, and a release that raised would leave the rest leaked.
    llvm::LLVMContext &Context = Services.Ctx->Context();
    llvm::Module &Mod          = Services.Ctx->Mod();

    llvm::Function *FiniAllFn = Mod.getFunction( "_V_fini_all" );
    if ( FiniAllFn == nullptr )
    {
        llvm::FunctionType *FnTy = llvm::FunctionType::get( Services.Ctx->Builder().getVoidTy(), false );
        FiniAllFn                = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, "_V_fini_all", &Mod );
    }
    if ( not FiniAllFn->empty() )
    {
        return true;
    }

    llvm::IRBuilder<> Shell{ llvm::BasicBlock::Create( Context, "entry", FiniAllFn ) };

    if ( Services.Build != nullptr )
    {
        for ( const std::string &Symbol : Backend::SynthesizeFiniAll( Services.Build->Units ) )
        {
            llvm::Function *FiniFn = Mod.getFunction( Symbol );
            if ( FiniFn == nullptr )
            {
                llvm::FunctionType *FnTy = llvm::FunctionType::get( Services.Ctx->Builder().getVoidTy(), false );
                FiniFn                   = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, Symbol, &Mod );
            }
            static_cast<void>( Shell.CreateCall( FiniFn ) );
        }
    }

    static_cast<void>( Shell.CreateRetVoid() );
    return true;
}

bool Volt::Backend::Llvm::EmitEntryPoint ( EmitterServices &Services )
{
    llvm::LLVMContext &Context = Services.Ctx->Context();
    llvm::Module &Mod          = Services.Ctx->Mod();

    // The C entry symbol (e.g. `main`). If no entry symbol is requested or if one
    // is already defined in the LLVM module, skip emitting the entry point.
    if ( Services.Options->EntrySymbol.empty() or Mod.getFunction( Services.Options->EntrySymbol ) != nullptr )
    {
        return true;
    }

    if ( not EmitInitAll( Services ) )
    {
        return false;
    }

    // Guarded behind the entry point for the same reason EmitSymbolNames is:
    // a library build has no program to be over, so it defines no teardown
    // sequence and the artifact it produces carries none.
    if ( not EmitFiniAll( Services ) )
    {
        return false;
    }

    // Same seam, same reason: a table of every `:symbol` in the build is a fact
    // only the build has, and the stdlib declares the one symbol that asks for
    // it. Guarded behind the entry point so a library build (no CRT entry, e.g.
    // the precompiled stdlib archive) never emits a second definition of it.
    if ( not EmitSymbolNames( Services ) )
    {
        return false;
    }

    // The Volt free function the C runtime hands control to
    // (source/Lib/Prelude.vl's `__volt_entry`, by default). DeclareAll has
    // already emitted its `llvm::Function` by the time Finalize reaches this
    // seam, exactly like any other free function.
    llvm::Function *EntryFn = nullptr;
    if ( Services.Build != nullptr and Services.Build->Types != nullptr )
    {
        if ( const MiddleEnd::TypeSystem::Member *Entry =
                 Services.Build->Types->LookupFunction( Services.Options->EntryFunction );
             Entry != nullptr )
        {
            EntryFn = Services.Functions->FunctionFor( *Entry, MiddleEnd::TypeSystem::NominalId{}, {} );
        }
    }
    if ( EntryFn == nullptr )
    {
        static_cast<void>( Services.Diag->Fail( "llvm: entry function '" + Services.Options->EntryFunction +
                                                "' not found — the stdlib prelude must declare it" ) );
        return false;
    }

    llvm::Type *ExitCodeTy = llvm::Type::getInt32Ty( Context );
    llvm::Type *ArgcTy     = llvm::Type::getInt32Ty( Context );
    llvm::Type *ArgvTy     = llvm::PointerType::getUnqual( Context );

    llvm::FunctionType *MainTy = llvm::FunctionType::get( ExitCodeTy, { ArgcTy, ArgvTy }, false );
    llvm::Function *MainFn =
        llvm::Function::Create( MainTy, llvm::Function::ExternalLinkage, Services.Options->EntrySymbol, &Mod );

    llvm::IRBuilder<> Shell{ llvm::BasicBlock::Create( Context, "entry", MainFn ) };
    llvm::Value *ExitCode = Shell.CreateCall( EntryFn, {}, "exit.code" );
    static_cast<void>( Shell.CreateRet( ExitCode ) );
    return true;
}
