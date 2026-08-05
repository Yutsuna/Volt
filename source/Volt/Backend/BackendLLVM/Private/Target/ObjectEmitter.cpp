// ObjectEmitter.cpp — the `--emit obj` artifact, and the linker's input.

#include "Target/TargetPipeline.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <string>
#include <system_error>

bool Volt::Backend::Llvm::TargetPipeline::EmitObjectFile ( std::string_view Path )
{
    std::error_code Error;
    llvm::raw_fd_ostream Stream( Path, Error, llvm::sys::fs::OF_None );
    if ( Error )
    {
        static_cast<void>( Services->Diag->Fail( "llvm: could not open '" + std::string( Path ) +
                                                 "' for object emission: " + Error.message() ) );
        return false;
    }

    llvm::legacy::PassManager CodegenPasses;
    if ( Services->Ctx->Machine().addPassesToEmitFile( CodegenPasses, Stream, nullptr, llvm::CodeGenFileType::ObjectFile ) )
    {
        static_cast<void>( Services->Diag->Fail( "llvm: target '" + Services->Ctx->Machine().getTargetTriple().str() +
                                                 "' cannot emit an object file" ) );
        return false;
    }

    CodegenPasses.run( Services->Ctx->Mod() );
    Stream.flush();
    return not Stream.has_error();
}
