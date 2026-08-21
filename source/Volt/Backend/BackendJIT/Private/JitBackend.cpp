// JitBackend.cpp — the protocol, over BackendLlvmIr and JitCompiler.
//
// Begin / EmitUnit / Finalize are the same three phases every backend has; what
// differs is the tail. Instead of optimising a module and writing an object,
// this one moves the module into ORC and calls into it.
//
// Three emission options are what make the IR runnable in-process, and each is
// forced by something ORC does rather than chosen:
//
//   - the triple and data layout come from LLJIT, because the code has to be
//     typed for the machine that will actually execute it;
//   - no TargetMachine, because nothing here runs addPassesToEmitFile;
//   - ETlsAccess::Accessor, because JIT-linked code cannot carry TLS
//     relocations without an ORC runtime whose version is not ours to pin
//     (UnwindTransport.hpp states the contract, jit.md the reasoning).
//
// Verification *is* turned on here, unlike the AOT path: BackendLLVM has its own
// verify step that names the offending function, and a JIT has nowhere to report
// from — a malformed module handed to ORC crashes inside the JIT rather than
// producing a diagnostic.

#include "Volt/BackendJIT/JitBackend.hpp"

#include "JitCompiler.hpp"

#include "Volt/BackendCore/UnwindTransport.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"
#include "Volt/BackendLlvmIr/LlvmAccess.hpp"
#include "Volt/Core/Support/PhaseTimer.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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

    JitCompiler Compiler;
    std::optional<Ir::IrGenerator> Gen;

    GenerationId Generation = 0;
    bool bMaterialised      = false;

    // What each unit contributed to the running program, kept so a reload of
    // that unit has something to compare its replacement against. Recorded per
    // unit rather than per build because that is the granularity a reload
    // works at.
    std::map<std::uint32_t, std::vector<Ir::IrGenerator::UnitSymbol>> UnitSymbols;
    std::map<std::uint32_t, std::vector<Ir::IrGenerator::UnitShape>> UnitShapes;

    // The slot address for a symbol, resolved once. A reload writes through it
    // and a later reload of the same symbol writes through the same one — the
    // slot never moves, which is the entire point of it.
    std::map<std::string, std::uintptr_t> Slots;

    std::string Error;

    // Everything the emission needs to be runnable in-process, in one place:
    // a reload builds a second emission and every one of these has to match,
    // or the replacement is compiled for a different machine than the code it
    // is replacing.
    [[nodiscard]] Ir::IrOptions MakeIrOptions () const
    {
        Ir::IrOptions Opts;
        Opts.Granularity = Options.bPerUnitModules ? Ir::EModuleGranularity::PerUnit : Ir::EModuleGranularity::Whole;
        Opts.Tls         = Ir::ETlsAccess::Accessor;
        Opts.Linkage     = Options.bIndirectLinkage ? Ir::ELinkage::Indirect : Ir::ELinkage::Direct;

        // No inline-eligible exception to the skip: a skipped unit's code is in a
        // dylib and the JIT calls it there. The AOT path wants the opposite because
        // it can inline across the boundary; nothing here can.
        Opts.SkipUnitsBelow             = Options.SkipUnitsBelow;
        Opts.bDefineInlineEligibleBelow = false;

        // The artifact is loaded, not linked, so any seam it contains was filled
        // in for its own build and cannot be reused.
        Opts.bDefineCompilerSeamUnits = true;

        Opts.TargetTriple       = Compiler.TargetTriple();
        Opts.DataLayout         = Compiler.DataLayoutString();
        Opts.bNeedTargetMachine = false;

        Opts.EntryFunction = Options.EntryFunction;
        Opts.EntrySymbol   = Options.EntrySymbol;
        Opts.bVerify       = true;

        return Opts;
    }

    [[nodiscard]] bool Failed () const
    {
        return not Error.empty() or ( Gen.has_value() and Gen->Failed() );
    }

    [[nodiscard]] std::string Message () const
    {
        if ( not Error.empty() )
        {
            return Error;
        }
        return Gen.has_value() ? std::string( Gen->Error() ) : std::string{};
    }

    EEmitStatus Fail ( std::string InMessage )
    {
        if ( Error.empty() )
        {
            Error = std::move( InMessage );
        }
        return EEmitStatus::Error;
    }
};

Volt::Backend::Jit::JitBackend::JitBackend () : Impl( std::make_unique<State>() )
{
}

Volt::Backend::Jit::JitBackend::~JitBackend () = default;

Volt::Backend::Jit::JitBackend::JitBackend ( JitBackend && ) noexcept = default;

Volt::Backend::Jit::JitBackend &Volt::Backend::Jit::JitBackend::operator=( JitBackend && ) noexcept = default;

void Volt::Backend::Jit::JitBackend::SetOptions ( JitOptions InOptions )
{
    Impl->Options = std::move( InOptions );
}

void Volt::Backend::Jit::JitBackend::Begin ( const BackendInput &Input )
{
    Impl->Build = &Input;

    if ( Input.Types == nullptr )
    {
        static_cast<void>( Impl->Fail( "jit: the build carries no TypeStore" ) );
        return;
    }

    std::string Error;
    if ( not Impl->Compiler.Init( Impl->Options.CompileThreads, Error ) )
    {
        static_cast<void>( Impl->Fail( std::move( Error ) ) );
        return;
    }

    Ir::IrOptions Gen = Impl->MakeIrOptions();

    Impl->Gen.emplace( std::move( Gen ) );
    Impl->Gen->Begin( Input );
}

Volt::Backend::EEmitStatus Volt::Backend::Jit::JitBackend::EmitUnit ( const UnitView &Unit )
{
    if ( Impl->Failed() or not Impl->Gen.has_value() )
    {
        return EEmitStatus::Error;
    }

    const EEmitStatus Status = Impl->Gen->EmitUnit( Unit );
    if ( Status == EEmitStatus::Ok )
    {
        Impl->UnitSymbols[Unit.Ordinal] = Impl->Gen->LastUnitSymbols();
        Impl->UnitShapes[Unit.Ordinal]  = Impl->Gen->LastUnitShapes();
    }
    return Status;
}

Volt::Backend::EmitResult Volt::Backend::Jit::JitBackend::Finalize ()
{
    const auto MakeFailure = [this] ()
    { return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Impl->Message() }; };

    if ( Impl->Failed() or not Impl->Gen.has_value() )
    {
        return MakeFailure();
    }

    if ( Impl->Gen->Finish() != EEmitStatus::Ok )
    {
        return MakeFailure();
    }

    // Before anything runs: the accessor's provider is this process, and the
    // width of the in-flight-exception buffer is a fact only the emission knows.
    Backend::SetUnwindStorageSize( Impl->Gen->UnwindStorageSize() );

    // Order matters. A named dylib is consulted before the process, so a
    // precompiled stdlib's definition of __volt_unwind_slots wins over the
    // compiler's own — which is what keeps JIT-ed code and that stdlib sharing
    // one copy of the transport state.
    std::string Error;
    for ( const std::string &Path : Impl->Options.Dylibs )
    {
        if ( not Impl->Compiler.AddDylib( Path, Error ) )
        {
            static_cast<void>( Impl->Fail( std::move( Error ) ) );
            return MakeFailure();
        }
    }
    if ( not Impl->Compiler.AddProcessSymbols( Error ) )
    {
        static_cast<void>( Impl->Fail( std::move( Error ) ) );
        return MakeFailure();
    }

    const Volt::Core::PhaseScope Timing( "backend.jit.add" );

    Impl->Generation = Impl->Compiler.OpenGeneration();
    if ( not Impl->Compiler.AddModules( Impl->Generation, Ir::TakeModules( *Impl->Gen ), Error ) )
    {
        static_cast<void>( Impl->Fail( std::move( Error ) ) );
        return MakeFailure();
    }

    Impl->bMaterialised = true;

    // A JIT build's artifact is the resident code itself: there is no file to
    // name, and naming one would be a lie the caller could act on.
    return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = {}, .Message = {} };
}

Volt::Backend::RunResult Volt::Backend::Jit::JitBackend::Run ( std::span<const std::string_view> ProgramArgs )
{
    if ( not Impl->bMaterialised )
    {
        return RunResult{
            .bOk = false, .Code = 1, .Message = Impl->Message().empty() ? "jit: nothing was materialised" : Impl->Message() };
    }

    std::uintptr_t Address = 0;
    std::string Error;
    {
        // ORC compiles lazily: this lookup is what forces the whole module
        // through codegen, so it is where a JIT's real cost shows up. Timed
        // separately from emission for exactly that reason.
        const Volt::Core::PhaseScope Timing( "backend.jit.materialize" );
        if ( not Impl->Compiler.Lookup( Impl->Options.EntrySymbol, Address, Error ) )
        {
            return RunResult{ .bOk = false, .Code = 1, .Message = std::move( Error ) };
        }
    }

    // argv has to outlive the call and be NUL-terminated in both senses: each
    // string, and the array. string_view guarantees neither, so it is copied.
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

    // The one unavoidable cast in the module: ORC hands back an address, and
    // calling it is the entire point.
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

    if ( not Impl->bMaterialised )
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

    // --- Emit the replacement ------------------------------------------------
    //
    // A whole IrGenerator, not a reuse of the running one: the running one was
    // built over the old type store and gave its modules and its context to ORC
    // at Finalize. This is a second, complete emission that happens to define
    // one unit's bodies and declare everything else.
    Ir::IrOptions Opts       = Impl->MakeIrOptions();
    Opts.bReplaceUnit        = true;
    Opts.EntrySymbol         = {};
    Opts.bDefineSlotAccessor = false;

    // The running program already has both seams filled in, and its copies are
    // the ones every unit calls. A second definition here would be dead code at
    // best and a divergent symbol table at worst.
    Opts.bDefineCompilerSeamUnits = false;

    Ir::IrGenerator Replacement( std::move( Opts ) );
    Replacement.Begin( Build );
    if ( Replacement.EmitUnit( Unit ) != EEmitStatus::Ok or Replacement.Finish() != EEmitStatus::Ok )
    {
        return Failed( "jit: the replacement for '" + std::string( Unit.Path ) +
                       "' did not emit: " + std::string( Replacement.Error() ) );
    }

    // --- Decide whether it may be swapped in ---------------------------------
    //
    // Both checks are one-sided on purpose. The JIT cannot inspect the stack,
    // so it cannot know whether a frame of the old code is live or whether an
    // instance of a changed type exists. It answers the stricter question it
    // *can* answer, and is therefore sometimes needlessly refusing and never
    // wrongly accepting. The fallback is a full restart, which this target
    // makes cheap.
    const std::vector<Ir::IrGenerator::UnitSymbol> NewSymbols = Replacement.LastUnitSymbols();
    for ( const Ir::IrGenerator::UnitSymbol &Was : KnownSymbols->second )
    {
        const auto Still = std::find_if( NewSymbols.begin(), NewSymbols.end(),
                                         [&Was] ( const Ir::IrGenerator::UnitSymbol &Now ) { return Now.Name == Was.Name; } );
        if ( Still == NewSymbols.end() )
        {
            // Every caller elsewhere in the build still reaches this symbol
            // through its slot, and there is nothing left to point the slot at.
            return Refuse( "jit: '" + Was.Name + "' is gone from the new '" + std::string( Unit.Path ) +
                           "' — a function callers already resolved cannot simply disappear" );
        }
        if ( Still->Signature != Was.Signature )
        {
            // The callers that were *not* recompiled still push the old shape
            // onto the stack. Nothing here can find them — a JIT cannot walk
            // the callers of a symbol any more than it can walk live frames —
            // so this refuses on the signature alone, with no condition
            // attached: stricter than it has to be, and never wrong.
            return Refuse( "jit: '" + Was.Name + "' changed shape (" + Was.Signature + " -> " + Still->Signature +
                           ") — callers compiled against the old one are still running" );
        }
    }

    const auto KnownShapes = Impl->UnitShapes.find( Unit.Ordinal );
    if ( KnownShapes != Impl->UnitShapes.end() and Replacement.LastUnitShapes() != KnownShapes->second )
    {
        return Refuse( "jit: a type declared in '" + std::string( Unit.Path ) +
                       "' changed size, alignment or existence — instances of it are already laid out the old way" );
    }

    // --- Swap it in ----------------------------------------------------------
    GenerationId Gen = 0;
    std::string Error;
    if ( not Impl->Compiler.OpenReplacement( Gen, Error ) )
    {
        return Failed( std::move( Error ) );
    }
    if ( not Impl->Compiler.AddModules( Gen, Ir::TakeModules( Replacement ), Error ) )
    {
        return Failed( std::move( Error ) );
    }

    // The old generation is deliberately *not* dropped. Removing it would unmap
    // its executable memory, and a frame still running there would die on the
    // next instruction. The cost is resident memory — tens of kilobytes per
    // reload — and it is the right trade against a crash.
    std::size_t Patched = 0;
    for ( const Ir::IrGenerator::UnitSymbol &Symbol : NewSymbols )
    {
        std::uintptr_t &Slot = Impl->Slots[Symbol.Name];
        if ( Slot == 0 and not Impl->Compiler.Lookup( Ir::SlotNameOf( Symbol.Name ), Slot, Error ) )
        {
            return Failed( "jit: '" + Symbol.Name + "' has no indirection slot to repoint: " + Error );
        }

        std::uintptr_t Address = 0;
        if ( not Impl->Compiler.LookupIn( Gen, Symbol.Name, Address, Error ) )
        {
            return Failed( std::move( Error ) );
        }

        // The only window in the whole mechanism. One aligned pointer store, so
        // a thread calling through this slot right now reads either the old
        // address or the new one and never a mixture of the two.
        *reinterpret_cast<std::uintptr_t *>( Slot ) = Address; // NOLINT(performance-no-int-to-ptr)
        ++Patched;
    }

    Impl->UnitSymbols[Unit.Ordinal] = NewSymbols;
    Impl->UnitShapes[Unit.Ordinal]  = Replacement.LastUnitShapes();
    return ReloadResult{ .Status = EReloadStatus::Ok, .Message = {}, .PatchedSymbols = Patched };
}

Volt::Backend::RunResult Volt::Backend::Jit::JitBackend::EvalUnit ( const UnitView &Unit )
{
    static_cast<void>( Unit );
    return RunResult{ .bOk = false, .Code = 1, .Message = "jit: incremental evaluation is not implemented yet" };
}

std::uintptr_t Volt::Backend::Jit::JitBackend::LookupSymbol ( std::string_view Mangled )
{
    std::uintptr_t Address = 0;
    std::string Error;
    if ( not Impl->Compiler.Lookup( Mangled, Address, Error ) )
    {
        return 0;
    }
    return Address;
}
