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

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCTargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdlib>

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
                        // The REPL's `:asm` reads back the bytes ORC mapped;
                        // without this the target registry has a printer and
                        // no decoder, and lookupTarget succeeds only to fail
                        // one call later.
                        ( void )llvm::InitializeNativeTargetDisassembler();
                    } );
}

// llvm::toString consumes the Error, which is exactly what every call site here
// wants: the message survives, the Error does not.
std::string Consume ( llvm::Error Err )
{
    return llvm::toString( std::move( Err ) );
}

// Every module that reaches ORC, appended to the file VOLT_JIT_DUMP_IR names.
// The JIT has no `--emit ir` of its own — its modules are handed straight to
// ORC and never written anywhere — so without this the only way to see what
// the JIT actually compiled is a debugger.
void DumpIfAsked ( const llvm::Module &Mod )
{
    const char *Where = std::getenv( "VOLT_JIT_DUMP_IR" );
    if ( Where == nullptr )
    {
        return;
    }

    std::error_code Ec;
    llvm::raw_fd_ostream Out( std::string( Where ), Ec, llvm::sys::fs::OF_Append );
    if ( not Ec )
    {
        Mod.print( Out, nullptr );
    }
}

// A module holding only declarations resolves nothing and materialises
// nothing, so adding it to a dylib costs a symbol-table scan and buys an entry
// nobody can ever look up.
bool DefinesAnything ( const llvm::Module &Mod )
{
    for ( const llvm::Function &Fn : Mod )
    {
        if ( not Fn.isDeclaration() )
        {
            return true;
        }
    }
    for ( const llvm::GlobalVariable &Var : Mod.globals() )
    {
        if ( Var.hasInitializer() )
        {
            return true;
        }
    }
    return false;
}

} // namespace

struct Volt::Backend::Jit::JitCompiler::Impl
{

    std::unique_ptr<llvm::orc::LLJIT> Jit;
    std::map<GenerationId, llvm::orc::ResourceTrackerSP> Trackers;

    // Only for a generation that owns one; a plain generation is a tracker
    // over the main dylib and is absent here.
    std::map<GenerationId, llvm::orc::JITDylib *> Dylibs;
    GenerationId NextGeneration = 1;

    // The one context every module of this session was typed in. Held here
    // rather than moved into the first ThreadSafeModule because a later
    // generation — a reload, a REPL line — has to be opened in the same one.
    llvm::orc::ThreadSafeContext Ctx;
    bool bContextAdopted = false;

    // The text of the last batch of modules added, kept only while a consumer
    // has asked for it. A REPL's `:ir` is that consumer, and it is the only
    // one — a `volt run` would render every module it compiles for nothing.
    bool bRecordIr = false;
    std::string LastIrText;
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

bool Volt::Backend::Jit::JitCompiler::OpenReplacement ( GenerationId &OutGen, std::string &OutError )
{
    const GenerationId Id = P->NextGeneration++;

    llvm::Expected<llvm::orc::JITDylib &> Made = P->Jit->createJITDylib( "volt.gen." + std::to_string( Id ) );
    if ( not Made )
    {
        OutError = "jit: could not open a generation dylib: " + Consume( Made.takeError() );
        return false;
    }

    // Itself first, then the main dylib: what the replacement defines is what
    // the replacement's own calls should reach, and everything else — the
    // stdlib, the other units, the process — resolves exactly as before.
    Made->addToLinkOrder( P->Jit->getMainJITDylib() );

    P->Dylibs[Id]   = &*Made;
    P->Trackers[Id] = Made->createResourceTracker();
    OutGen          = Id;
    return true;
}

bool Volt::Backend::Jit::JitCompiler::AddModules ( GenerationId Gen, Ir::OwnedModules Modules, std::string &OutError )
{
    const auto Found = P->Trackers.find( Gen );
    if ( Found == P->Trackers.end() )
    {
        OutError = "jit: no such generation";
        return false;
    }
    if ( Modules.Modules.empty() )
    {
        OutError = "jit: the emission produced no module";
        return false;
    }

    // One batch is one unit's emission, so the record holds the *last* unit
    // rather than everything since recording was turned on. A caller that
    // wants a unit kept out of it — the REPL's own echo line, which is not a
    // line anybody typed — turns recording off around it, and this leaves what
    // is already held alone.
    if ( P->bRecordIr )
    {
        P->LastIrText.clear();
    }

    // The context is adopted once and kept: every module of this session shares
    // it, which is what lets one module call a function another one defines. A
    // later batch — a reload, a REPL line — arrives without one and is opened
    // in the context already held.
    if ( Modules.Context != nullptr )
    {
        P->Ctx = llvm::orc::ThreadSafeContext( std::move( Modules.Context ) );
    }
    else if ( not P->bContextAdopted )
    {
        OutError = "jit: the first batch of modules brought no context";
        return false;
    }
    P->bContextAdopted = true;

    for ( std::unique_ptr<llvm::Module> &Mod : Modules.Modules )
    {
        if ( Mod == nullptr or not DefinesAnything( *Mod ) )
        {
            continue;
        }

        const std::string Name( Mod->getName() );
        DumpIfAsked( *Mod );
        if ( P->bRecordIr )
        {
            llvm::raw_string_ostream Text( P->LastIrText );
            Mod->print( Text, nullptr );
        }
        if ( llvm::Error Err = P->Jit->addIRModule( Found->second, llvm::orc::ThreadSafeModule( std::move( Mod ), P->Ctx ) ) )
        {
            OutError = "jit: could not add the module '" + Name + "': " + Consume( std::move( Err ) );
            return false;
        }
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

bool Volt::Backend::Jit::JitCompiler::LookupIn ( GenerationId Gen,
                                                 std::string_view Symbol,
                                                 std::uintptr_t &OutAddr,
                                                 std::string &OutError )
{
    const auto Found = P->Dylibs.find( Gen );
    if ( Found == P->Dylibs.end() )
    {
        return Lookup( Symbol, OutAddr, OutError );
    }

    llvm::Expected<llvm::orc::ExecutorAddr> Addr = P->Jit->lookup( *Found->second, Symbol );
    if ( not Addr )
    {
        OutError = "jit: '" + std::string( Symbol ) + "' did not resolve: " + Consume( Addr.takeError() );
        return false;
    }
    OutAddr = static_cast<std::uintptr_t>( Addr->getValue() );
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

std::size_t Volt::Backend::Jit::JitCompiler::LiveGenerations () const
{
    return P->Trackers.size();
}

void Volt::Backend::Jit::JitCompiler::RecordIr ( const bool bEnable )
{
    // Deliberately does not clear: turning recording off is how a caller
    // *excludes* a unit from the record, and clearing would throw away the
    // very thing it was protecting.
    P->bRecordIr = bEnable;
}

std::string Volt::Backend::Jit::JitCompiler::LastIr () const
{
    return P->LastIrText;
}

std::string Volt::Backend::Jit::JitCompiler::Disassemble ( const std::uintptr_t Address,
                                                           const std::size_t MaxBytes,
                                                           std::string &OutError ) const
{
    if ( P->Jit == nullptr or Address == 0 or MaxBytes == 0 )
    {
        OutError = "jit: nothing to disassemble";
        return {};
    }

    InitialiseNativeTarget();

    const llvm::Triple TheTriple = P->Jit->getTargetTriple();

    std::string Why;
    const llvm::Target *Machine = llvm::TargetRegistry::lookupTarget( TheTriple, Why );
    if ( Machine == nullptr )
    {
        OutError = "jit: no disassembler for '" + TheTriple.str() + "': " + Why;
        return {};
    }

    // Every one of these owns a piece of the target description, and the
    // context below borrows three of them — so they are declared here, in
    // destruction order, and outlive it.
    const std::unique_ptr<llvm::MCRegisterInfo> Registers( Machine->createMCRegInfo( TheTriple ) );
    if ( Registers == nullptr )
    {
        OutError = "jit: the target has no register description";
        return {};
    }

    const llvm::MCTargetOptions Options;
    const std::unique_ptr<llvm::MCAsmInfo> AsmInfo( Machine->createMCAsmInfo( *Registers, TheTriple, Options ) );
    const std::unique_ptr<llvm::MCSubtargetInfo> Subtarget( Machine->createMCSubtargetInfo( TheTriple, "", "" ) );
    const std::unique_ptr<llvm::MCInstrInfo> Instructions( Machine->createMCInstrInfo() );
    if ( AsmInfo == nullptr or Subtarget == nullptr or Instructions == nullptr )
    {
        OutError = "jit: the target description is incomplete";
        return {};
    }

    llvm::MCContext Context( TheTriple, AsmInfo.get(), Registers.get(), Subtarget.get() );
    const std::unique_ptr<llvm::MCDisassembler> Decoder( Machine->createMCDisassembler( *Subtarget, Context ) );
    const std::unique_ptr<llvm::MCInstPrinter> Printer(
        Machine->createMCInstPrinter( TheTriple, AsmInfo->getAssemblerDialect(), *AsmInfo, *Instructions, *Registers ) );
    if ( Decoder == nullptr or Printer == nullptr )
    {
        OutError = "jit: this build of LLVM has no decoder for '" + TheTriple.str() + "'";
        return {};
    }

    // The bytes ORC mapped, read as bytes. There is no other way to see them:
    // JIT-linked code is never written to a file, so what a debugger would
    // read from an object is only available here, in memory, at this address.
    const llvm::ArrayRef<std::uint8_t> Code(
        reinterpret_cast<const std::uint8_t *>( Address ), // NOLINT(performance-no-int-to-ptr)
        MaxBytes );

    std::string Out;
    llvm::raw_string_ostream Text( Out );

    std::uint64_t Offset = 0;
    while ( Offset < MaxBytes )
    {
        llvm::MCInst Instruction;
        std::uint64_t Size = 0;

        const llvm::MCDisassembler::DecodeStatus Status =
            Decoder->getInstruction( Instruction, Size, Code.slice( Offset ), Address + Offset, llvm::nulls() );
        if ( Status != llvm::MCDisassembler::Success or Size == 0 )
        {
            break;
        }

        Text << llvm::format_hex( Address + Offset, 18 ) << "  ";
        Printer->printInst( &Instruction, Address + Offset, "", *Subtarget, Text );
        Text << '\n';

        Offset += Size;

        // A function ends at its return, and reading past one would decode
        // whatever the linker happened to place next as if it belonged here.
        // Padding between functions is not always a valid instruction, so the
        // decode failure above catches the rest.
        if ( Instructions->get( Instruction.getOpcode() ).isReturn() )
        {
            break;
        }
    }

    if ( Out.empty() )
    {
        OutError = "jit: nothing decoded at that address";
    }
    return Out;
}
