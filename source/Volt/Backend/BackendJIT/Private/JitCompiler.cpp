// JitCompiler.cpp — see JitCompiler.hpp.
//
// Every ORC call that can fail returns llvm::Error or llvm::Expected. None of
// them are allowed past this file: an Error that is neither consumed nor
// returned aborts the process on destruction, so each one is turned into a
// message here, at the point where there is still context to name what failed.

#include "JitCompiler.hpp"

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/IRPartitionLayer.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
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
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// Where a lazy call-through lands when the body it was supposed to compile
// could not be compiled.
//
// LLLazyJIT defaults this to address 0, which turns a compile failure into a
// jump to a null pointer — a SIGSEGV with the trampoline's frame on the stack
// and nothing to read. It is reached *from JIT-ed code*, in the middle of the
// user's program, with that call's arguments still in registers, so there is
// nothing sensible to return: the only honest thing left is to say what
// happened and stop.
//
// bVerify is what keeps this unreachable in practice — a malformed module is
// refused at emission, where there is a diagnostic to give.
[[noreturn]] void LazyCompileFailed ()
{
    llvm::errs() << "jit: a function could not be compiled when it was first called; the program cannot continue\n";
    std::abort();
}

// What one lazy compile compiles: the function that was asked for, plus every
// function this module defines that its code *names*, transitively.
//
// LLVM ships two policies and neither is right here. `compileWholeModule` is
// eager with extra steps. `compileRequested` — one function per partition — is
// the obvious reading of "lazy", and it is what made laziness a bet rather than
// a win: each partition clones the module skeleton, so compiling n functions one
// at a time costs O(n^2)-ish, and a program that ends up calling everything it
// defines measured *68% slower* than eager.
//
// Taking the callees closes that gap without giving up the thing worth having.
// A module nothing reaches is still never compiled, and a function nothing
// calls is still never compiled — that is where all the winning happens, and it
// is untouched. What changes is only the shape of the work once a module *is*
// reached: one partition holding a call tree instead of n partitions holding a
// function each.
//
// Reached from an *instruction*, which is the line this draws. A function named
// by code that runs is one the caller is about to use: the callee of a direct
// call, and equally the body half of a closure pair, whose address is taken by
// a store in the very block that goes on to call it. A function named only by a
// *global initialiser* is not — that is a vtable, and which of its entries a
// `dyn Trait` will reach is exactly what nobody knows until it is reached. Those
// keep their stubs and stay lazy, which is what makes dynamic dispatch pay only
// for the methods it actually dispatches to.
void CollectReferenced ( const llvm::Value *From, std::vector<const llvm::Function *> &Out )
{
    if ( const auto *Fn = llvm::dyn_cast<llvm::Function>( From ); Fn != nullptr )
    {
        Out.push_back( Fn );
        return;
    }

    // A function pointer folded into a constant — an aggregate built inline for
    // a closure pair, an expression around it — is still named by this
    // instruction, so it is still evidence.
    if ( const auto *Expr = llvm::dyn_cast<llvm::Constant>( From ); Expr != nullptr )
    {
        for ( const llvm::Use &Operand : Expr->operands() )
        {
            CollectReferenced( Operand.get(), Out );
        }
    }
}

// What one lazy compile compiles, gathered transitively.
std::optional<llvm::orc::IRPartitionLayer::GlobalValueSet>
PartitionWithCallees ( llvm::orc::IRPartitionLayer::GlobalValueSet Requested )
{
    llvm::orc::IRPartitionLayer::GlobalValueSet Partition = std::move( Requested );

    std::vector<const llvm::Function *> Pending;
    for ( const llvm::GlobalValue *Value : Partition )
    {
        if ( const auto *Fn = llvm::dyn_cast<llvm::Function>( Value ); Fn != nullptr and not Fn->isDeclaration() )
        {
            Pending.push_back( Fn );
        }
    }

    std::vector<const llvm::Function *> Named;
    while ( not Pending.empty() )
    {
        const llvm::Function *Fn = Pending.back();
        Pending.pop_back();

        for ( const llvm::Instruction &Inst : llvm::instructions( *Fn ) )
        {
            Named.clear();
            for ( const llvm::Use &Operand : Inst.operands() )
            {
                CollectReferenced( Operand.get(), Named );
            }

            for ( const llvm::Function *Referenced : Named )
            {
                // A declaration resolves through the dylib like any other
                // undefined symbol; there is no body here to put in a partition.
                if ( Referenced->isDeclaration() )
                {
                    continue;
                }
                if ( Partition.insert( Referenced ).second )
                {
                    Pending.push_back( Referenced );
                }
            }
        }
    }
    return Partition;
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

    // The same object as Jit when the session is lazy, null when it is not.
    // LLLazyJIT *is* an LLJIT and everything below goes through the base;
    // this exists solely for addLazyIRModule, which the base does not have.
    llvm::orc::LLLazyJIT *Lazy = nullptr;

    ECompilePolicy CompilePolicy = ECompilePolicy::Eager;

    // One batch, added together and removed together.
    struct Generation
    {

        // Null for a generation living in the main dylib — the common case,
        // and what OpenGeneration produces. Non-null only for a replacement,
        // which needs a symbol table of its own.
        llvm::orc::JITDylib *Dylib = nullptr;

        // Null for a lazy batch, which has no tracker to be given: the modules
        // go to the dylib's default tracker (JitCompiler.hpp, DropGeneration).
        llvm::orc::ResourceTrackerSP Tracker;

        bool bLazy = false;
    };

    std::map<GenerationId, Generation> Generations;
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

bool Volt::Backend::Jit::JitCompiler::Init ( const unsigned CompileThreads, const ECompilePolicy Wanted, std::string &OutError )
{
    InitialiseNativeTarget();

    if ( Wanted == ECompilePolicy::Lazy )
    {
        llvm::orc::LLLazyJITBuilder Builder;
        Builder.setNumCompileThreads( CompileThreads );
        Builder.setLazyCompileFailureAddr( llvm::orc::ExecutorAddr::fromPtr( &LazyCompileFailed ) );

        if ( llvm::Expected<std::unique_ptr<llvm::orc::LLLazyJIT>> Built = Builder.create() )
        {
            P->Lazy = Built->get();
            P->Lazy->setPartitionFunction( PartitionWithCallees );
            P->Jit           = std::move( *Built );
            P->CompilePolicy = ECompilePolicy::Lazy;
            return true;
        }
        else
        {
            // Lazy needs trampolines LLVM only has for some architectures, and
            // a machine without them still deserves to run the program. The
            // Error is consumed rather than reported: falling back is not a
            // failure, and Policy() is how a caller learns it happened.
            llvm::consumeError( Built.takeError() );
        }
    }

    llvm::orc::LLJITBuilder Builder;
    Builder.setNumCompileThreads( CompileThreads );

    llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> Built = Builder.create();
    if ( not Built )
    {
        OutError = "jit: could not create an LLJIT: " + Consume( Built.takeError() );
        return false;
    }

    P->Jit           = std::move( *Built );
    P->CompilePolicy = ECompilePolicy::Eager;
    return true;
}

Volt::Backend::Jit::ECompilePolicy Volt::Backend::Jit::JitCompiler::Policy () const
{
    return P->CompilePolicy;
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

    Impl::Generation &Gen = P->Generations[Id];
    Gen.bLazy             = P->CompilePolicy == ECompilePolicy::Lazy;

    // A lazy batch is handed to a dylib, not to a tracker, so making one here
    // would be an object that only ever answers a question nobody may ask.
    if ( not Gen.bLazy )
    {
        Gen.Tracker = P->Jit->getMainJITDylib().createResourceTracker();
    }
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
    // Also add the replacement dylib to the main dylib's link order so any
    // fresh globals declared on a line that redefines a function are visible
    // to subsequent lines in the session.
    P->Jit->getMainJITDylib().addToLinkOrder( *Made );

    // Eager unconditionally — the header says why. Note that this is a
    // property of the generation and not of the session: a lazy `volt run`
    // that later reloads gets a lazy boot generation and eager replacements,
    // which is the intended mix rather than an inconsistency.
    Impl::Generation &Gen = P->Generations[Id];
    Gen.Dylib             = &*Made;
    Gen.Tracker           = Made->createResourceTracker();
    Gen.bLazy             = false;

    OutGen = Id;
    return true;
}

bool Volt::Backend::Jit::JitCompiler::AddModules ( GenerationId Gen, Ir::OwnedModules Modules, std::string &OutError )
{
    const auto Found = P->Generations.find( Gen );
    if ( Found == P->Generations.end() )
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
        llvm::orc::ThreadSafeModule Safe( std::move( Mod ), P->Ctx );

        // The one line the whole policy comes down to. Lazy goes to the dylib
        // because addLazyIRModule has no ResourceTracker overload — which is
        // exactly what makes a lazy generation undroppable.
        llvm::Error Err =
            Found->second.bLazy
                ? P->Lazy->addLazyIRModule( Found->second.Dylib == nullptr ? P->Jit->getMainJITDylib() : *Found->second.Dylib,
                                            std::move( Safe ) )
                : P->Jit->addIRModule( Found->second.Tracker, std::move( Safe ) );
        if ( Err )
        {
            OutError = "jit: could not add the module '" + Name + "': " + Consume( std::move( Err ) );
            return false;
        }
    }
    return true;
}

bool Volt::Backend::Jit::JitCompiler::DropGeneration ( GenerationId Gen, std::string &OutError )
{
    const auto Found = P->Generations.find( Gen );
    if ( Found == P->Generations.end() )
    {
        OutError = "jit: no such generation";
        return false;
    }
    if ( Found->second.Tracker == nullptr )
    {
        OutError = "jit: a lazily compiled generation cannot be removed";
        return false;
    }
    if ( llvm::Error Err = Found->second.Tracker->remove() )
    {
        OutError = "jit: could not remove a generation: " + Consume( std::move( Err ) );
        return false;
    }
    P->Generations.erase( Found );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::LookupIn ( GenerationId Gen,
                                                 std::string_view Symbol,
                                                 std::uintptr_t &OutAddr,
                                                 std::string &OutError )
{
    const auto Found = P->Generations.find( Gen );
    if ( Found == P->Generations.end() or Found->second.Dylib == nullptr )
    {
        return Lookup( Symbol, OutAddr, OutError );
    }

    llvm::Expected<llvm::orc::ExecutorAddr> Addr = P->Jit->lookup( *Found->second.Dylib, Symbol );
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
    return P->Generations.size();
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
