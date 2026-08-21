// JitCompiler.cpp — see JitCompiler.hpp.
//
// Every ORC call that can fail returns llvm::Error or llvm::Expected. None of
// them are allowed past this file: an Error that is neither consumed nor
// returned aborts the process on destruction, so each one is turned into a
// message here, at the point where there is still context to name what failed.

#include "JitCompiler.hpp"

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace
{

// Same reason ModuleContext guards it: the registry is process-global and not
// re-entrant, and the JIT may be the first thing in the process to touch it.
void InitialiseNativeTarget ()
{
    static std::once_flag Once;
    std::call_once( Once,
                    [] ()
                    {
                        llvm::InitializeNativeTarget();
                        llvm::InitializeNativeTargetAsmPrinter();
                        llvm::InitializeNativeTargetAsmParser();
                    } );
}

// llvm::toString consumes the Error, which is exactly what every call site here
// wants: the message survives, the Error does not.
std::string Consume ( llvm::Error Err )
{
    return llvm::toString( std::move( Err ) );
}

} // namespace

struct Volt::Backend::Jit::JitCompiler::Impl
{

    std::unique_ptr<llvm::orc::LLJIT> Jit;
    std::map<GenerationId, llvm::orc::ResourceTrackerSP> Trackers;
    GenerationId NextGeneration = 1;
};

Volt::Backend::Jit::JitCompiler::JitCompiler () : P( std::make_unique<Impl>() )
{
}

Volt::Backend::Jit::JitCompiler::~JitCompiler () = default;

bool Volt::Backend::Jit::JitCompiler::Init ( unsigned CompileThreads, std::string &OutError )
{
    InitialiseNativeTarget();

    llvm::orc::LLJITBuilder Builder;
    Builder.setNumCompileThreads( CompileThreads );

    llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> Built = Builder.create();
    if ( not Built )
    {
        OutError = "jit: could not create an LLJIT: " + Consume( Built.takeError() );
        return false;
    }

    P->Jit = std::move( *Built );
    return true;
}

std::string Volt::Backend::Jit::JitCompiler::TargetTriple () const
{
    return P->Jit == nullptr ? std::string{} : P->Jit->getTargetTriple().str();
}

std::string Volt::Backend::Jit::JitCompiler::DataLayoutString () const
{
    return P->Jit == nullptr ? std::string{} : P->Jit->getDataLayout().getStringRepresentation();
}

Volt::Backend::Jit::GenerationId Volt::Backend::Jit::JitCompiler::OpenGeneration ()
{
    const GenerationId Id = P->NextGeneration++;
    P->Trackers[Id]       = P->Jit->getMainJITDylib().createResourceTracker();
    return Id;
}

bool Volt::Backend::Jit::JitCompiler::AddModule ( GenerationId Gen, Ir::OwnedModule Module, std::string &OutError )
{
    const auto Found = P->Trackers.find( Gen );
    if ( Found == P->Trackers.end() )
    {
        OutError = "jit: no such generation";
        return false;
    }
    if ( Module.Module == nullptr or Module.Context == nullptr )
    {
        OutError = "jit: the emission produced no module";
        return false;
    }

    // ThreadSafeModule takes both halves, which is the whole reason
    // ModuleContext owns its LLVMContext by pointer.
    llvm::orc::ThreadSafeModule Safe( std::move( Module.Module ), std::move( Module.Context ) );
    if ( llvm::Error Err = P->Jit->addIRModule( Found->second, std::move( Safe ) ) )
    {
        OutError = "jit: could not add the module: " + Consume( std::move( Err ) );
        return false;
    }
    return true;
}

bool Volt::Backend::Jit::JitCompiler::DropGeneration ( GenerationId Gen, std::string &OutError )
{
    const auto Found = P->Trackers.find( Gen );
    if ( Found == P->Trackers.end() )
    {
        OutError = "jit: no such generation";
        return false;
    }
    if ( llvm::Error Err = Found->second->remove() )
    {
        OutError = "jit: could not remove a generation: " + Consume( std::move( Err ) );
        return false;
    }
    P->Trackers.erase( Found );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::Lookup ( std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError )
{
    llvm::Expected<llvm::orc::ExecutorAddr> Found = P->Jit->lookup( Symbol );
    if ( not Found )
    {
        OutError = "jit: symbol '" + std::string( Symbol ) + "' did not resolve: " + Consume( Found.takeError() );
        return false;
    }
    OutAddr = static_cast<std::uintptr_t>( Found->getValue() );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::AddDylib ( std::string_view Path, std::string &OutError )
{
    const char Prefix = P->Jit->getDataLayout().getGlobalPrefix();

    llvm::Expected<std::unique_ptr<llvm::orc::DynamicLibrarySearchGenerator>> Gen =
        llvm::orc::DynamicLibrarySearchGenerator::Load( std::string( Path ).c_str(), Prefix );
    if ( not Gen )
    {
        OutError = "jit: could not load '" + std::string( Path ) + "': " + Consume( Gen.takeError() );
        return false;
    }

    P->Jit->getMainJITDylib().addGenerator( std::move( *Gen ) );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::AddProcessSymbols ( std::string &OutError )
{
    const char Prefix = P->Jit->getDataLayout().getGlobalPrefix();

    llvm::Expected<std::unique_ptr<llvm::orc::DynamicLibrarySearchGenerator>> Gen =
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess( Prefix );
    if ( not Gen )
    {
        OutError = "jit: could not open the process's own symbols: " + Consume( Gen.takeError() );
        return false;
    }

    P->Jit->getMainJITDylib().addGenerator( std::move( *Gen ) );
    return true;
}
