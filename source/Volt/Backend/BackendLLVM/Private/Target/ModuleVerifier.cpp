// ModuleVerifier.cpp — llvm::verifyModule, with the offending function named.

#include "Target/TargetPipeline.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

bool Volt::Backend::Llvm::TargetPipeline::VerifyModule ()
{
    // Volt's own contract (rules/core-ast.md) guarantees the middle-end never
    // hands the emitter something it cannot type or resolve; a broken module at
    // this point is an emitter bug, not a Volt source error, so the offending
    // function is named rather than the diagnostic being left to whatever
    // llvm::verifyModule prints on its own.
    std::string Report;
    llvm::raw_string_ostream Stream( Report );
    if ( not llvm::verifyModule( Services->Ctx->Mod(), &Stream ) )
    {
        return true;
    }

    for ( const llvm::Function &Fn : Services->Ctx->Mod() )
    {
        std::string FunctionReport;
        llvm::raw_string_ostream FunctionStream( FunctionReport );
        if ( llvm::verifyFunction( Fn, &FunctionStream ) )
        {
            static_cast<void>( Services->Diag->Fail( "llvm: module verification failed in '" +
                                                     std::string( Fn.getName() ) + "': " + FunctionStream.str() ) );
            return false;
        }
    }

    // The verifier found the module broken but no single function is at fault (a
    // malformed global, an ill-typed metadata node, ...) — report the
    // module-level text verbatim rather than guessing which function.
    static_cast<void>( Services->Diag->Fail( "llvm: module verification failed: " + Stream.str() ) );
    return false;
}
