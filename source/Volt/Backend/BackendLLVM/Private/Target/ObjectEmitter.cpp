// ObjectEmitter.cpp — the `--emit obj` artifact, and the linker's input.

#include "Target/TargetPipeline.hpp"

#include "Volt/BackendCore/DiagnosticSink.hpp"

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

    // One section per function and per datum, so the link has something to
    // discard at. It matters most for the artifact this same emitter produces
    // for the stdlib cache: an archive's unit of extraction is the *member*, and
    // that archive holds exactly one, so referencing a single stdlib function
    // pulls in every other one. Without `--gc-sections` having sections to work
    // on, a `puts` program carries the whole library's text; with them, the
    // linker keeps what it can reach and drops the rest.
    //
    // Set on the machine rather than at construction (ModuleContext.cpp) on
    // purpose: that machine is shared with the JIT, which links nothing and
    // would only pay the extra section headers.
    Services->Machine->Options.FunctionSections = true;
    Services->Machine->Options.DataSections     = true;

    llvm::legacy::PassManager CodegenPasses;
    if ( Services->Machine->addPassesToEmitFile( CodegenPasses, Stream, nullptr, llvm::CodeGenFileType::ObjectFile ) )
    {
        static_cast<void>( Services->Diag->Fail( "llvm: target '" + Services->Machine->getTargetTriple().str() +
                                                 "' cannot emit an object file" ) );
        return false;
    }

    CodegenPasses.run( *Services->Mod );
    Stream.flush();
    return not Stream.has_error();
}
