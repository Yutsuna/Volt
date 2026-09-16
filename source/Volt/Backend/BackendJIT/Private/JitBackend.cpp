// JitBackend.cpp — the execution and reload coordinator, over IJitQueue.
//
// Begin / EmitUnit / Finalize are the same three phases every backend has; what
// differs is the tail. Instead of writing an object, this backend executes the code
// in-process via an IJitQueue engine (LLVM ORC, or an alternative execution queue).
//
// This coordinator manages:
//   - Process execution, entry point invocation, argument marshaling
//   - Unwind exception buffer storage & transport accessors
//   - Indirection slot tables (@volt.fn.*) and atomic store patching
//   - Dynamic dispatch mutable vtables and vtable patching
//   - Reload verification (signature equality, type layout stability)
//   - REPL evaluation & incremental session state
//
// ZERO LLVM headers: the queue abstraction isolates all machine compilation details.

#include "Volt/BackendJIT/JitBackend.hpp"

#include "Volt/BackendCore/InitAllSynthesizer.hpp"
#include "Volt/BackendCore/UnwindTransport.hpp"
#include "Volt/BackendJIT/IJitQueue.hpp"
#include "Volt/Core/Support/PhaseTimer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef DEBUG_NO_STATIC_ASSERT
static_assert( Volt::Backend::TargetBackend<Volt::Backend::Jit::JitBackend> );
#endif

struct Volt::Backend::Jit::JitBackend::State
{

    JitOptions Options;
    const BackendInput *Build = nullptr;

    std::unique_ptr<IJitQueue> Queue;

    GenerationId Generation = 0;
    bool bMaterialised      = false;

    // What each unit contributed to the running program, kept so a reload of
    // that unit has something to compare its replacement against. Recorded per
    // unit rather than per build because that is the granularity a reload
    // works at.
    std::map<std::uint32_t, std::vector<CompiledUnitMeta::SymbolDef>> UnitSymbols;
    std::map<std::uint32_t, std::vector<CompiledUnitMeta::TypeShape>> UnitShapes;

    // The slot address for a symbol, resolved once. A reload writes through it
    // and a later reload of the same symbol writes through the same one — the
    // slot never moves, which is the entire point of it.
    std::map<std::string, std::uintptr_t, std::less<>> Slots;

    // Where dynamic dispatch reads its callees, and the address of each array,
    // resolved once for the same reason Slots is.
    std::vector<CompiledUnitMeta::VTableEntry> VTables;
    std::map<std::string, std::uintptr_t, std::less<>> VTableAddresses;

    // Every symbol this session has already defined, so a line can be asked the
    // one question that decides where its module goes: does it redefine
    // something?
    std::set<std::string, std::less<>> Defined;

    // The subset of Defined that carries an indirection slot: everything this
    // session emitted itself, and nothing that came out of a dylib.
    std::set<std::string, std::less<>> Slotted;

    // How wide the in-flight-exception buffer was when the session started.
    std::size_t BootUnwindStorage = 0;

    // The calling thread's transport slot table, resolved through the JIT.
    void **Transport = nullptr;

    std::string Error;

    [[nodiscard]] bool PatchSlots ( GenerationId Into,
                                    const std::vector<CompiledUnitMeta::SymbolDef> &Symbols,
                                    std::size_t &OutPatched,
                                    std::string &OutError )
    {
        for ( const CompiledUnitMeta::SymbolDef &Symbol : Symbols )
        {
            std::uintptr_t &Slot = Slots[Symbol.Name];

            if ( Slot == 0 )
            {
                const std::string SlotName = "volt.fn." + Symbol.Name;
                if ( not Queue->LookupIn( Into, SlotName, Slot, OutError ) and not Queue->Lookup( SlotName, Slot, OutError ) )
                {
                    OutError = "jit: '" + Symbol.Name + "' has no indirection slot to repoint: " + OutError;
                    return false;
                }
            }

            std::uintptr_t Address = 0;
            if ( not Queue->LookupIn( Into, Symbol.Name, Address, OutError ) )
            {
                return false;
            }

            std::atomic_ref<std::uintptr_t>( *reinterpret_cast<std::uintptr_t *>( Slot ) ) // NOLINT(performance-no-int-to-ptr)
                .store( Address, std::memory_order_relaxed );
            ++OutPatched;
        }
        return true;
    }

    [[nodiscard]] bool
    PatchVTables ( GenerationId Into, const std::vector<CompiledUnitMeta::SymbolDef> &Symbols, std::string &OutError )
    {
        for ( const CompiledUnitMeta::VTableEntry &Entry : VTables )
        {
            const auto Moved =
                std::find_if( Symbols.begin(), Symbols.end(),
                              [&Entry] ( const CompiledUnitMeta::SymbolDef &Symbol ) { return Symbol.Name == Entry.Function; } );
            if ( Moved == Symbols.end() )
            {
                continue;
            }

            std::uintptr_t &Base = VTableAddresses[Entry.VTable];
            if ( Base == 0 and not Queue->Lookup( Entry.VTable, Base, OutError ) )
            {
                OutError = "jit: '" + Entry.VTable + "' holds a pointer to '" + Entry.Function +
                           "' and cannot be found to repoint it: " + OutError;
                return false;
            }

            std::uintptr_t Address = 0;
            if ( not Queue->LookupIn( Into, Entry.Function, Address, OutError ) )
            {
                return false;
            }

            std::atomic_ref<std::uintptr_t>(
                reinterpret_cast<std::uintptr_t *>( Base )[Entry.Slot] ) // NOLINT(performance-no-int-to-ptr)
                .store( Address, std::memory_order_relaxed );
        }
        return true;
    }

    [[nodiscard]] std::uint32_t *ExceptionTag ()
    {
        if ( Transport == nullptr )
        {
            std::uintptr_t Accessor = 0;
            std::string Ignored;
            if ( not Queue->Lookup( UnwindTransport::SlotAccessorSymbol, Accessor, Ignored ) )
            {
                return nullptr;
            }
            using SlotsFn = void *( * )();
            Transport     = static_cast<void **>( reinterpret_cast<SlotsFn>( Accessor )() ); // NOLINT(performance-no-int-to-ptr)
        }
        return static_cast<std::uint32_t *>( Transport[UnwindTransport::SlotTableTagIndex] );
    }

    void ResetExceptionTag ()
    {
        if ( std::uint32_t *Tag = ExceptionTag() )
        {
            *Tag = UnwindTransport::NoExceptionTag;
        }
    }
};

Volt::Backend::Jit::JitBackend::JitBackend () : Impl( std::make_unique<State>() )
{
    Impl->Queue = CreateOrcJitQueue();
}

Volt::Backend::Jit::JitBackend::~JitBackend ()                                                      = default;
Volt::Backend::Jit::JitBackend::JitBackend ( JitBackend && ) noexcept                               = default;
Volt::Backend::Jit::JitBackend &Volt::Backend::Jit::JitBackend::operator=( JitBackend && ) noexcept = default;

void Volt::Backend::Jit::JitBackend::SetOptions ( JitOptions InOptions )
{
    Impl->Options = std::move( InOptions );
}

void Volt::Backend::Jit::JitBackend::SetQueue ( std::unique_ptr<IJitQueue> InQueue )
{
    Impl->Queue = std::move( InQueue );
}

void Volt::Backend::Jit::JitBackend::Begin ( const BackendInput &Input )
{
    if ( not Impl->Queue )
    {
        Impl->Queue = CreateOrcJitQueue();
    }

    Impl->Build = &Input;
    Impl->Error.clear();
    Impl->VTables.clear();

    SessionOptions Wanted;
    Wanted.CompileThreads = Impl->Options.CompileThreads;
    Wanted.Policy         = Impl->Options.bLazyCompilation ? ECompilePolicy::Lazy : ECompilePolicy::Eager;
    Wanted.OptLevel       = Impl->Options.OptLevel;

    std::string Error;
    if ( not Impl->Queue->Init( Wanted, Error ) )
    {
        Impl->Error = Error;
        return;
    }

    for ( const std::string &Path : Impl->Options.Dylibs )
    {
        if ( not Impl->Queue->AddDylib( Path, Error ) )
        {
            Impl->Error = Error;
            return;
        }
    }

    if ( not Impl->Queue->AddProcessSymbols( Error ) )
    {
        Impl->Error = Error;
        return;
    }

    Impl->Queue->Begin( Input, Impl->Options );
    Impl->BootUnwindStorage = Impl->Queue->UnwindStorageSize();
}

Volt::Backend::EEmitStatus Volt::Backend::Jit::JitBackend::EmitUnit ( const UnitView &Unit )
{
    if ( not Impl->Error.empty() or not Impl->Queue )
    {
        return EEmitStatus::Error;
    }

    CompiledUnitMeta Meta;
    const EEmitStatus Status = Impl->Queue->EmitUnit( Unit, Meta );
    if ( Status != EEmitStatus::Ok )
    {
        return Status;
    }

    Impl->UnitSymbols[Unit.Ordinal] = std::move( Meta.Symbols );
    Impl->UnitShapes[Unit.Ordinal]  = std::move( Meta.Shapes );
    for ( auto &VTab : Meta.VTables )
    {
        Impl->VTables.push_back( std::move( VTab ) );
    }
    return EEmitStatus::Ok;
}

Volt::Backend::EmitResult Volt::Backend::Jit::JitBackend::Finalize ()
{
    if ( not Impl->Error.empty() )
    {
        return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Impl->Error };
    }
    if ( not Impl->Queue )
    {
        return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = "jit: queue was not created" };
    }

    Impl->Generation = Impl->Queue->OpenGeneration();
    CompiledUnitMeta Meta;
    const EmitResult Emitted = Impl->Queue->Finalize( Impl->Generation, Meta );
    if ( Emitted.Status != EEmitStatus::Ok )
    {
        return Emitted;
    }

    Backend::SetUnwindStorageSize( Impl->Queue->UnwindStorageSize() );
    Impl->BootUnwindStorage = Impl->Queue->UnwindStorageSize();

    for ( const std::string &Symbol : Meta.DefinedSymbols )
    {
        Impl->Defined.insert( Symbol );
    }

    for ( const auto &[Ordinal, Syms] : Impl->UnitSymbols )
    {
        for ( const auto &Sym : Syms )
        {
            Impl->Defined.insert( Sym.Name );
            if ( Impl->Options.bIndirectLinkage )
            {
                Impl->Slotted.insert( Sym.Name );
            }
        }
    }

    for ( const auto &VTab : Meta.VTables )
    {
        Impl->VTables.push_back( VTab );
    }

    if ( not Impl->Options.EntrySymbol.empty() )
    {
        std::uintptr_t Address = 0;
        std::string Error;
        const Volt::Core::PhaseScope Timing( "backend.jit.materialize" );
        if ( not Impl->Queue->Lookup( Impl->Options.EntrySymbol, Address, Error ) )
        {
            return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Error };
        }
    }

    Impl->bMaterialised = true;
    return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = "<jit>", .Message = {} };
}

Volt::Backend::RunResult Volt::Backend::Jit::JitBackend::Run ( std::span<const std::string_view> ProgramArgs )
{
    if ( not Impl->bMaterialised or not Impl->Queue )
    {
        return RunResult{ .bOk = false, .Code = 1, .Message = "jit: Run called before Finalize" };
    }

    std::uintptr_t Address = 0;
    std::string Error;
    if ( not Impl->Queue->Lookup( Impl->Options.EntrySymbol, Address, Error ) )
    {
        return RunResult{ .bOk = false, .Code = 1, .Message = Error };
    }

    std::vector<std::string> Owned;
    Owned.reserve( ProgramArgs.size() );
    for ( const std::string_view Arg : ProgramArgs )
    {
        Owned.emplace_back( Arg );
    }

    std::vector<char *> Argv;
    Argv.reserve( Owned.size() + 1 );
    for ( std::string &Arg : Owned )
    {
        Argv.push_back( Arg.data() );
    }
    Argv.push_back( nullptr );

    using EntryFn = int ( * )( int, char ** );
    EntryFn Entry = reinterpret_cast<EntryFn>( Address ); // NOLINT(performance-no-int-to-ptr)

    const int Code = Entry( static_cast<int>( Owned.size() ), Argv.data() );
    return RunResult{ .bOk = true, .Code = Code, .Message = {} };
}

Volt::Backend::ReloadResult Volt::Backend::Jit::JitBackend::Reload ( const BackendInput &Build, const UnitView &Unit )
{
    const auto Refuse = [] ( std::string Why )
    { return ReloadResult{ .Status = EReloadStatus::Refused, .Message = std::move( Why ), .PatchedSymbols = 0 }; };
    const auto Failed = [] ( std::string Why )
    { return ReloadResult{ .Status = EReloadStatus::Error, .Message = std::move( Why ), .PatchedSymbols = 0 }; };

    if ( not Impl->bMaterialised or not Impl->Queue )
    {
        return Failed( "jit: nothing has been materialised, so there is nothing to reload into" );
    }
    if ( not Impl->Options.bPerUnitModules or not Impl->Options.bIndirectLinkage )
    {
        return Refuse( "jit: hot reload needs per-unit modules and indirect linkage; this session was built with neither" );
    }

    const auto KnownSymbols = Impl->UnitSymbols.find( Unit.Ordinal );
    if ( KnownSymbols == Impl->UnitSymbols.end() )
    {
        return Refuse( "jit: unit " + std::to_string( Unit.Ordinal ) + " was never emitted by this session" );
    }

    CompiledUnitMeta NewMeta;
    std::string Error;
    const auto IsAlreadyDefined   = [this] ( std::string_view Sym ) { return Impl->Defined.contains( Sym ); };
    const auto HasIndirectionSlot = [this] ( std::string_view Sym ) { return Impl->Slotted.contains( Sym ); };

    if ( not Impl->Queue->PrepareReplacement( Build, Unit, IsAlreadyDefined, HasIndirectionSlot, NewMeta, Error ) )
    {
        return Failed( "jit: the replacement for '" + std::string( Unit.Path ) + "' did not emit: " + Error );
    }

    for ( const CompiledUnitMeta::SymbolDef &Was : KnownSymbols->second )
    {
        const auto Still = std::find_if( NewMeta.Symbols.begin(), NewMeta.Symbols.end(),
                                         [&Was] ( const CompiledUnitMeta::SymbolDef &Now ) { return Now.Name == Was.Name; } );
        if ( Still == NewMeta.Symbols.end() )
        {
            Impl->Queue->DiscardReplacement();
            return Refuse( "jit: '" + Was.Name + "' is gone from the new '" + std::string( Unit.Path ) +
                           "' — a function callers already resolved cannot simply disappear" );
        }
        if ( Still->Signature != Was.Signature )
        {
            Impl->Queue->DiscardReplacement();
            return Refuse( "jit: '" + Was.Name + "' changed shape (" + Was.Signature + " -> " + Still->Signature +
                           ") — callers compiled against the old one are still running" );
        }
    }

    const auto KnownShapes = Impl->UnitShapes.find( Unit.Ordinal );
    if ( KnownShapes != Impl->UnitShapes.end() and NewMeta.Shapes != KnownShapes->second )
    {
        Impl->Queue->DiscardReplacement();
        return Refuse( "jit: a type declared in '" + std::string( Unit.Path ) +
                       "' changed size, alignment or existence — instances of it are already laid out the old way" );
    }

    for ( const CompiledUnitMeta::VTableEntry &Now : NewMeta.VTables )
    {
        const auto Was =
            std::find_if( Impl->VTables.begin(), Impl->VTables.end(), [&Now] ( const CompiledUnitMeta::VTableEntry &Held )
                          { return Held.VTable == Now.VTable and Held.Slot == Now.Slot; } );
        if ( Was == Impl->VTables.end() )
        {
            Impl->Queue->DiscardReplacement();
            return Refuse( "jit: '" + std::string( Unit.Path ) + "' needs slot " + std::to_string( Now.Slot ) + " of '" +
                           Now.VTable +
                           "', which the running program never built — a trait implemented for the first "
                           "time cannot be reached by a program compiled before it existed" );
        }
        if ( Was->Function != Now.Function )
        {
            Impl->Queue->DiscardReplacement();
            return Refuse( "jit: slot " + std::to_string( Now.Slot ) + " of '" + Now.VTable + "' held '" + Was->Function +
                           "', but the replacement puts '" + Now.Function +
                           "' there — dynamic dispatch compiled against the old array reads the old slot number" );
        }
    }

    GenerationId Gen = 0;
    if ( not Impl->Queue->OpenReplacement( Gen, Error ) )
    {
        Impl->Queue->DiscardReplacement();
        return Failed( std::move( Error ) );
    }

    if ( not Impl->Queue->CommitReplacement( Gen, Error ) )
    {
        return Failed( std::move( Error ) );
    }

    std::size_t Patched = 0;
    if ( not Impl->PatchSlots( Gen, NewMeta.Symbols, Patched, Error ) )
    {
        return Failed( std::move( Error ) );
    }
    if ( not Impl->PatchVTables( Gen, NewMeta.Symbols, Error ) )
    {
        return Failed( std::move( Error ) );
    }

    Impl->UnitSymbols[Unit.Ordinal] = std::move( NewMeta.Symbols );
    Impl->UnitShapes[Unit.Ordinal]  = std::move( NewMeta.Shapes );

    return ReloadResult{ .Status = EReloadStatus::Ok, .Message = {}, .PatchedSymbols = Patched };
}

Volt::Backend::RunResult Volt::Backend::Jit::JitBackend::EvalUnit ( const BackendInput &Build, const UnitView &Unit )
{
    const auto Failed = [] ( std::string Why ) { return RunResult{ .bOk = false, .Code = 1, .Message = std::move( Why ) }; };

    if ( not Impl->bMaterialised or not Impl->Queue )
    {
        return Failed( "jit: EvalUnit called before Finalize" );
    }

    const auto IsAlreadyDefined   = [this] ( std::string_view Sym ) { return Impl->Defined.contains( Sym ); };
    const auto HasIndirectionSlot = [this] ( std::string_view Sym ) { return Impl->Slotted.contains( Sym ); };

    GenerationId Gen = 0;
    bool bRedefines  = false;
    CompiledUnitMeta Meta;
    std::string Error;

    if ( not Impl->Queue->CompileEvalUnit( Build, Unit, Gen, bRedefines, IsAlreadyDefined, HasIndirectionSlot, Meta,
                                           Impl->BootUnwindStorage, Error ) )
    {
        return Failed( std::move( Error ) );
    }

    if ( bRedefines )
    {
        std::size_t Patched = 0;
        if ( not Impl->PatchSlots( Gen, Meta.Symbols, Patched, Error ) )
        {
            return Failed( std::move( Error ) );
        }
        if ( not Impl->PatchVTables( Gen, Meta.Symbols, Error ) )
        {
            return Failed( std::move( Error ) );
        }
    }

    for ( const CompiledUnitMeta::VTableEntry &Entry : Meta.VTables )
    {
        if ( std::find( Impl->VTables.begin(), Impl->VTables.end(), Entry ) == Impl->VTables.end() )
        {
            Impl->VTables.push_back( Entry );
        }
    }

    for ( const std::string &Symbol : Meta.DefinedSymbols )
    {
        Impl->Defined.insert( Symbol );
    }
    for ( const CompiledUnitMeta::SymbolDef &Symbol : Meta.Symbols )
    {
        Impl->Slotted.insert( Symbol.Name );
    }

    if ( UnitHasInit( Unit ) )
    {
        const std::string InitSymbol = "_V_init_" + std::to_string( Unit.Ordinal );
        std::uintptr_t Address       = 0;
        if ( not Impl->Queue->LookupIn( Gen, InitSymbol, Address, Error ) )
        {
            return Failed( "repl: initialiser '" + InitSymbol + "' did not resolve: " + Error );
        }

        using InitFn = void ( * )();
        reinterpret_cast<InitFn>( Address )(); // NOLINT(performance-no-int-to-ptr)
    }

    if ( const std::uint32_t *Tag = Impl->ExceptionTag(); Tag != nullptr and *Tag != UnwindTransport::NoExceptionTag )
    {
        const std::uint32_t Value = *Tag;
        Impl->ResetExceptionTag();
        return Failed( "unhandled exception (tag " + std::to_string( Value ) + ")" );
    }

    return RunResult{ .bOk = true, .Code = 0, .Message = {} };
}

std::uintptr_t Volt::Backend::Jit::JitBackend::LookupSymbol ( std::string_view Mangled )
{
    if ( not Impl->bMaterialised or not Impl->Queue )
    {
        return 0;
    }
    std::uintptr_t Address = 0;
    std::string Ignored;
    return Impl->Queue->Lookup( Mangled, Address, Ignored ) ? Address : 0;
}

bool Volt::Backend::Jit::JitBackend::ProbeUnit ( const BackendInput &Build,
                                                 const UnitView &Unit,
                                                 std::string *OutIr,
                                                 std::string &OutError )
{
    if ( not Impl->Queue )
    {
        OutError = "jit: queue not initialized";
        return false;
    }
    return Impl->Queue->ProbeUnit( Build, Unit, OutIr, OutError );
}

std::string Volt::Backend::Jit::JitBackend::LastUnitIr () const
{
    return Impl->Queue ? Impl->Queue->LastIr() : std::string{};
}

void Volt::Backend::Jit::JitBackend::RecordIr ( bool bEnable )
{
    if ( Impl->Queue )
    {
        Impl->Queue->RecordIr( bEnable );
    }
}

std::string Volt::Backend::Jit::JitBackend::Disassemble ( std::uintptr_t Address, std::size_t MaxBytes )
{
    if ( not Impl->Queue )
    {
        return {};
    }
    std::string Ignored;
    return Impl->Queue->Disassemble( Address, MaxBytes, Ignored );
}

Volt::Backend::IJitBackend::BenchResult
Volt::Backend::Jit::JitBackend::BenchUnit ( const BackendInput &Build, const UnitView &Unit, std::size_t Iterations )
{
    const auto Failed = [] ( std::string Why )
    { return BenchResult{ .bOk = false, .Message = std::move( Why ), .Iterations = 0, .TotalNanos = 0, .BestNanos = 0 }; };

    if ( not Impl->bMaterialised or not Impl->Queue )
    {
        return Failed( "jit: BenchUnit called before Finalize" );
    }
    if ( Iterations == 0 )
    {
        return Failed( "repl: iteration count must be greater than zero" );
    }
    if ( not UnitHasInit( Unit ) )
    {
        return Failed( "jit: this unit has no top-level statements to run" );
    }

    const auto IsAlreadyDefined   = [this] ( std::string_view Sym ) { return Impl->Defined.contains( Sym ); };
    const auto HasIndirectionSlot = [this] ( std::string_view Sym ) { return Impl->Slotted.contains( Sym ); };

    GenerationId Gen = 0;
    std::string Error;
    if ( not Impl->Queue->CompileBenchUnit( Build, Unit, Gen, IsAlreadyDefined, HasIndirectionSlot, Error ) )
    {
        return Failed( std::move( Error ) );
    }

    const std::string InitSymbol = "_V_init_" + std::to_string( Unit.Ordinal );
    std::uintptr_t Address       = 0;
    if ( not Impl->Queue->LookupIn( Gen, InitSymbol, Address, Error ) )
    {
        std::string Ignored;
        ( void )Impl->Queue->DropGeneration( Gen, Ignored );
        return Failed( "repl: initialiser '" + InitSymbol + "' did not resolve: " + Error );
    }

    using InitFn = void ( * )();
    auto Init    = reinterpret_cast<InitFn>( Address ); // NOLINT(performance-no-int-to-ptr)

    std::uint64_t TotalNanos = 0;
    std::uint64_t BestNanos  = std::numeric_limits<std::uint64_t>::max();

    for ( std::size_t I = 0; I < Iterations; ++I )
    {
        const auto Before = std::chrono::steady_clock::now();
        Init();
        const auto After = std::chrono::steady_clock::now();

        if ( const std::uint32_t *Tag = Impl->ExceptionTag(); Tag != nullptr and *Tag != UnwindTransport::NoExceptionTag )
        {
            const std::uint32_t Value = *Tag;
            Impl->ResetExceptionTag();
            std::string Ignored;
            ( void )Impl->Queue->DropGeneration( Gen, Ignored );
            return Failed( "unhandled exception during benchmark (tag " + std::to_string( Value ) + ")" );
        }

        const auto Nanos =
            static_cast<std::uint64_t>( std::chrono::duration_cast<std::chrono::nanoseconds>( After - Before ).count() );
        TotalNanos += Nanos;
        BestNanos = std::min( BestNanos, Nanos );
    }

    std::string Ignored;
    ( void )Impl->Queue->DropGeneration( Gen, Ignored );

    return BenchResult{ .bOk = true, .Message = {}, .Iterations = Iterations, .TotalNanos = TotalNanos, .BestNanos = BestNanos };
}

std::size_t Volt::Backend::Jit::JitBackend::LiveGenerations () const
{
    return Impl->Queue ? Impl->Queue->LiveGenerations() : 0;
}
