// IrEmitter.cpp — the `--emit ir` artifact.

#include "Target/TargetPipeline.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"

#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <system_error>

bool Volt::Backend::Llvm::TargetPipeline::EmitIrFile ( std::string_view Path )
{
    std::error_code Error;
    llvm::raw_fd_ostream Stream( Path, Error, llvm::sys::fs::OF_Text );
    if ( Error )
    {
        static_cast<void>(
            Services->Diag->Fail( "llvm: could not open '" + std::string( Path ) + "' for --emit ir: " + Error.message() ) );
        return false;
    }
    Services->Ctx->Mod().print( Stream, nullptr );
    return not Stream.has_error();
}
