// EntryPointEmitter.cpp — `_V_init_all`, and the C shim the runtime starts at.
//
// Both are synthesised rather than emitted from a Volt body, which is why they
// are hand-rolled here instead of going through BodyEmitter. What they
// deliberately do *not* do is decide anything: reporting an uncaught exception
// and choosing the exit status are `__volt_entry`'s own `begin/rescue` in
// source/Lib/Prelude.vl (rules/zero-hardcode.md). No field name, no type name,
// no byte of message enters C++ here.

#include "Functions/FunctionRegistry.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/Exception/ExceptionLowering.hpp"

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
        for ( std::size_t Index = 0; Index < Services.Build->Units.size(); ++Index )
        {
            const UnitView &Unit       = Services.Build->Units[Index];
            const std::string InitName = "_V_init_" + std::to_string( Unit.Ordinal );
            llvm::Function *InitFn     = Mod.getFunction( InitName );
            if ( InitFn == nullptr )
            {
                llvm::FunctionType *FnTy = llvm::FunctionType::get( Services.Ctx->Builder().getVoidTy(), false );
                InitFn                   = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, InitName, &Mod );
            }
            static_cast<void>( Shell.CreateCall( InitFn ) );

            if ( Index + 1 < Services.Build->Units.size() )
            {
                llvm::Value *Tag = Shell.CreateLoad( Int32Ty, Services.Exceptions->ExceptionTagSlot(), "exc.tag" );
                llvm::Value *Pending =
                    Shell.CreateICmpNE( Tag, llvm::ConstantInt::get( Int32Ty, Sema::NominalId::InvalidValue ), "exc.pending" );

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

    // The Volt free function the C runtime hands control to
    // (source/Lib/Prelude.vl's `__volt_entry`, by default). DeclareAll has
    // already emitted its `llvm::Function` by the time Finalize reaches this
    // seam, exactly like any other free function.
    llvm::Function *EntryFn = nullptr;
    if ( Services.Build != nullptr and Services.Build->Types != nullptr )
    {
        if ( const Sema::Member *Entry = Services.Build->Types->LookupFunction( Services.Options->EntryFunction );
             Entry != nullptr )
        {
            EntryFn = Services.Functions->FunctionFor( *Entry, Sema::NominalId{}, {} );
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
