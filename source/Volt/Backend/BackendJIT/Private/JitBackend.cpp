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

#include "Volt/BackendCore/InitAllSynthesizer.hpp"
#include "Volt/BackendCore/UnwindTransport.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"
#include "Volt/BackendLlvmIr/LlvmAccess.hpp"
#include "Volt/Core/Support/PhaseTimer.hpp"

#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
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

    // Every symbol this session has already defined, so a line can be asked the
    // one question that decides where its module goes: does it redefine
    // something? A line that does not is added to the main dylib like anything
    // else and every later line reaches it by ordinary lookup. A line that does
    // needs a dylib of its own, because ORC rejects a duplicate definition
    // inside one dylib — and then nothing reaches the new body by name at all,
    // only through the slot this patches.
    std::set<std::string> Defined;

    // The subset of Defined that carries an indirection slot: everything this
    // session emitted itself, and nothing that came out of a dylib. A later
    // line calls through the slot for these and by relocation for the rest.
    std::set<std::string> Slotted;

    // How wide the in-flight-exception buffer was when the session started.
    //
    // Kept because it cannot be changed afterwards. The provider allocates the
    // buffer the first time a thread calls the accessor (UnwindSlots.cpp, or
    // the equivalent inside a precompiled stdlib), reading the requested size
    // at that moment; a later SetUnwindStorageSize grows the request for a
    // thread that has not asked yet and does nothing at all for one that has.
    // A REPL has always asked by the time it evaluates a line, so this is the
    // width for the whole session and a line needing more has to be refused.
    std::size_t BootUnwindStorage = 0;

    // The calling thread's transport slot table, resolved through the JIT
    // rather than by calling this process's own accessor: a session that
    // loaded a precompiled stdlib reaches *that* copy of the slots, and
    // reading the compiler's own would be reading a different program's
    // exception state (UnwindTransport.hpp explains why there is only ever
    // one copy that matters).
    void **Transport = nullptr;

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

    // One unit, emitted alone against an already-running session: every other
    // unit declared, nothing else defined, no entry point and no seam.
    //
    // Shared by Reload and EvalUnit because they differ in exactly one bit.
    // A reload *replaces* a unit the running program already owns, so its
    // module-level storage is declared rather than defined (bReplaceUnit); an
    // evaluated line is a unit the session has never seen, so its storage is
    // its own to define, and every line before it is below the skip line.
    [[nodiscard]] Ir::IrOptions OneUnitOptions ( std::uint32_t Ordinal, bool bReplacing ) const
    {
        Ir::IrOptions Opts       = MakeIrOptions();
        Opts.bReplaceUnit        = bReplacing;
        Opts.EntrySymbol         = {};
        Opts.bDefineSlotAccessor = false;

        // The running program already has both seams filled in, and its copies
        // are the ones every unit calls. A second definition here would be dead
        // code at best and a divergent symbol table at worst.
        Opts.bDefineCompilerSeamUnits = false;

        // A replacement redefines a unit that is already below nothing — its
        // ordinal is wherever it always was. A new line is the highest ordinal
        // there is, so everything before it is code the session already holds.
        Opts.SkipUnitsBelow = bReplacing ? Opts.SkipUnitsBelow : Ordinal;

        // Monomorphisations are the one thing SkipUnitsBelow cannot cover: an
        // instantiation belongs to no unit, so an `Array<Int32>#push` first
        // reached on line 4 would be emitted again by every later line that
        // reaches it. Under PerUnit those carry external linkage, which makes
        // the second one a duplicate definition rather than a mergeable copy.
        Opts.IsAlreadyDefined = [this] ( std::string_view Symbol ) { return Defined.contains( std::string( Symbol ) ); };

        // Every earlier line sits below the skip line, but its code is resident
        // here rather than in an artifact, and it has a slot. Say so, or a
        // redefinition typed at the prompt never reaches its callers.
        Opts.HasIndirectionSlot = [this] ( std::string_view Symbol ) { return Slotted.contains( std::string( Symbol ) ); };
        return Opts;
    }

    // Point every named symbol's indirection slot at its definition inside
    // `Gen`. The one and only window in the whole mechanism, for a reload and
    // for a redefinition typed at a prompt alike.
    [[nodiscard]] bool PatchSlots ( GenerationId Into,
                                    const std::vector<Ir::IrGenerator::UnitSymbol> &Symbols,
                                    std::size_t &OutPatched,
                                    std::string &OutError )
    {
        for ( const Ir::IrGenerator::UnitSymbol &Symbol : Symbols )
        {
            std::uintptr_t &Slot = Slots[Symbol.Name];

            // Resolved once per symbol and then remembered, because after the
            // first time it is not findable by search: a slot is defined
            // beside the *first* definition of its function, so a redefinition
            // arriving in a dylib of its own would look for it in the wrong
            // place. The cache is not an optimisation here, it is the only
            // thing that still knows where the slot is.
            if ( Slot == 0 )
            {
                const std::string SlotName = Ir::SlotNameOf( Symbol.Name );
                if ( not Compiler.LookupIn( Into, SlotName, Slot, OutError ) and not Compiler.Lookup( SlotName, Slot, OutError ) )
                {
                    OutError = "jit: '" + Symbol.Name + "' has no indirection slot to repoint: " + OutError;
                    return false;
                }
            }

            std::uintptr_t Address = 0;
            if ( not Compiler.LookupIn( Into, Symbol.Name, Address, OutError ) )
            {
                return false;
            }

            // One aligned pointer store, so a thread calling through this slot
            // right now reads either the old address or the new one and never a
            // mixture of the two.
            *reinterpret_cast<std::uintptr_t *>( Slot ) = Address; // NOLINT(performance-no-int-to-ptr)
            ++OutPatched;
        }
        return true;
    }

    // The in-flight exception tag of the calling thread, or NoExceptionTag.
    // Null until the session has run something, which is also the only point
    // at which the accessor is guaranteed to resolve.
    [[nodiscard]] std::uint32_t *ExceptionTag ()
    {
        if ( Transport == nullptr )
        {
            std::uintptr_t Accessor = 0;
            std::string Ignored;
            if ( not Compiler.Lookup( UnwindTransport::SlotAccessorSymbol, Accessor, Ignored ) )
            {
                return nullptr;
            }
            using SlotsFn = void *( * )();
            Transport     = static_cast<void **>( reinterpret_cast<SlotsFn>( Accessor )() ); // NOLINT(performance-no-int-to-ptr)
        }
        return static_cast<std::uint32_t *>( Transport[UnwindTransport::SlotTableTagIndex] );
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
        for ( const Ir::IrGenerator::UnitSymbol &Symbol : Impl->UnitSymbols[Unit.Ordinal] )
        {
            Impl->Defined.insert( Symbol.Name );
            Impl->Slotted.insert( Symbol.Name );
        }
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
    Impl->BootUnwindStorage = Impl->Gen->UnwindStorageSize();

    // Per-unit lists miss `volt.shared` entirely, and that is where every
    // monomorphisation lives — exactly what a later line must not emit again.
    for ( const std::string &Symbol : Impl->Gen->DefinedSymbols() )
    {
        Impl->Defined.insert( Symbol );
    }

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
    Ir::IrGenerator Replacement( Impl->OneUnitOptions( Unit.Ordinal, /*bReplacing=*/true ) );
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
    if ( not Impl->PatchSlots( Gen, NewSymbols, Patched, Error ) )
    {
        return Failed( std::move( Error ) );
    }

    Impl->UnitSymbols[Unit.Ordinal] = NewSymbols;
    Impl->UnitShapes[Unit.Ordinal]  = Replacement.LastUnitShapes();
    return ReloadResult{ .Status = EReloadStatus::Ok, .Message = {}, .PatchedSymbols = Patched };
}

Volt::Backend::RunResult Volt::Backend::Jit::JitBackend::EvalUnit ( const BackendInput &Build, const UnitView &Unit )
{
    const auto Failed = [] ( std::string Why ) { return RunResult{ .bOk = false, .Code = 1, .Message = std::move( Why ) }; };

    if ( not Impl->bMaterialised )
    {
        return Failed( "jit: the session was never materialised, so there is nothing to evaluate into" );
    }
    if ( not Impl->Options.bPerUnitModules or not Impl->Options.bIndirectLinkage )
    {
        return Failed( "jit: incremental evaluation needs per-unit modules and indirect linkage; "
                       "this session was built with neither" );
    }

    // --- Emit this line, and nothing else ------------------------------------
    //
    // Everything below this unit's ordinal is a line the session has already
    // run: its code is resident, its module-level storage holds values the user
    // put there, and both are reached by declaration. That is precisely what
    // the skip line means, so a REPL needs no notion of its own.
    Ir::IrGenerator Line( Impl->OneUnitOptions( Unit.Ordinal, /*bReplacing=*/false ) );
    Line.Begin( Build );
    if ( Line.EmitUnit( Unit ) != EEmitStatus::Ok or Line.Finish() != EEmitStatus::Ok )
    {
        return Failed( "jit: this line did not emit: " + std::string( Line.Error() ) );
    }

    // --- Refuse what the session cannot carry --------------------------------
    //
    // The transport buffer was sized when the session started and cannot grow
    // afterwards (State::BootUnwindStorage says why). A line that raises
    // something wider would copy past the end of it, so it is refused before it
    // is ever added — the same one-sided doctrine as Reload's refusals:
    // sometimes needlessly strict, never wrong.
    if ( Line.UnwindStorageSize() > Impl->BootUnwindStorage )
    {
        return Failed( "repl: this line can raise a value of " + std::to_string( Line.UnwindStorageSize() ) +
                       " bytes, wider than the session's unwind buffer of " + std::to_string( Impl->BootUnwindStorage ) +
                       " bytes, which was fixed when the session started.\n"
                       "       -> :reset reopens a session sized for it, or declare the type in a file loaded at startup." );
    }

    // --- Add it, in the dylib the answer to one question picks ---------------
    //
    // Almost every line defines only names nobody has used yet — a fresh
    // `_V_init_N`, a fresh `_V_global_N_x`, a function typed for the first
    // time — and belongs in the main dylib, where every later line finds it by
    // ordinary lookup and nothing needs a search order at all.
    //
    // A line that *redefines* something cannot go there: ORC rejects a
    // duplicate definition inside one dylib. It gets a dylib of its own, and
    // then nothing reaches the new body by name — only the indirection slot
    // does, which is exactly what makes redefinition work for callers that
    // were compiled long ago and will never be recompiled.
    const std::vector<Ir::IrGenerator::UnitSymbol> Symbols = Line.LastUnitSymbols();

    const bool bRedefines = std::any_of( Symbols.begin(), Symbols.end(), [this] ( const Ir::IrGenerator::UnitSymbol &Symbol )
                                         { return Impl->Defined.contains( Symbol.Name ); } );

    GenerationId Gen = 0;
    std::string Error;
    if ( bRedefines )
    {
        if ( not Impl->Compiler.OpenReplacement( Gen, Error ) )
        {
            return Failed( std::move( Error ) );
        }
    }
    else
    {
        Gen = Impl->Compiler.OpenGeneration();
    }

    if ( not Impl->Compiler.AddModules( Gen, Ir::TakeModules( Line ), Error ) )
    {
        return Failed( std::move( Error ) );
    }

    // Only a redefinition has a slot to move. A symbol defined here for the
    // first time has its slot defined beside it, already pointing at it.
    if ( bRedefines )
    {
        std::size_t Patched = 0;
        if ( not Impl->PatchSlots( Gen, Symbols, Patched, Error ) )
        {
            return Failed( std::move( Error ) );
        }
    }

    for ( const std::string &Symbol : Line.DefinedSymbols() )
    {
        Impl->Defined.insert( Symbol );
    }
    for ( const Ir::IrGenerator::UnitSymbol &Symbol : Symbols )
    {
        Impl->Slotted.insert( Symbol.Name );
    }

    // --- Run its top-level statements ----------------------------------------
    //
    // When it has any: a line that declared a method and nothing else leaves an
    // empty top level, and nothing emits an initializer for one
    // (Backend::UnitHasInit). Looking the symbol up anyway would fail on an
    // absence that means "nothing to run", which is not an error.
    std::uint32_t *Tag = Impl->ExceptionTag();
    if ( UnitHasInit( Unit ) )
    {
        const std::string InitSymbol = "_V_init_" + std::to_string( Unit.Ordinal );

        std::uintptr_t Address = 0;
        if ( not Impl->Compiler.LookupIn( Gen, InitSymbol, Address, Error ) )
        {
            return Failed( std::move( Error ) );
        }

        if ( Tag != nullptr )
        {
            // Whatever a previous line left behind is not this line's business,
            // and a stale tag would make this one look like it raised.
            *Tag = UnwindTransport::NoExceptionTag;
        }

        using InitFn = void ( * )();
        reinterpret_cast<InitFn>( Address )(); // NOLINT(performance-no-int-to-ptr)
    }

    Impl->UnitSymbols[Unit.Ordinal] = Symbols;
    Impl->UnitShapes[Unit.Ordinal]  = Line.LastUnitShapes();

    // A raise that nobody rescued unwound out of the unit init and left the tag
    // set. It ends the *line*, never the session — that difference is the whole
    // point of a REPL, and it is why this returns a message rather than letting
    // the caller treat a non-zero code as fatal.
    if ( Tag != nullptr and *Tag != UnwindTransport::NoExceptionTag )
    {
        const std::uint32_t Raised = *Tag;
        *Tag                       = UnwindTransport::NoExceptionTag;

        std::string Named = "an exception";
        if ( Build.Types != nullptr and Raised < Build.Types->TypeCount() )
        {
            const MiddleEnd::TypeSystem::NominalId Id{ Raised };
            Named = "'" + std::string( Build.Types->Text( Build.Types->Type( Id ).Name ) ) + "'";
        }
        return Failed( "repl: " + Named + " was raised and never rescued" );
    }

    return RunResult{ .bOk = true, .Code = 0, .Message = {} };
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

bool Volt::Backend::Jit::JitBackend::ProbeUnit ( const BackendInput &Build,
                                                 const UnitView &Unit,
                                                 std::string *OutIr,
                                                 std::string &OutError )
{
    if ( not Impl->bMaterialised )
    {
        OutError = "jit: the session was never materialised, so there is nothing to probe against";
        return false;
    }

    // The same generator EvalUnit builds, with the same options, so that what
    // this reports is what evaluating the line would actually produce.
    Ir::IrGenerator Line( Impl->OneUnitOptions( Unit.Ordinal, /*bReplacing=*/false ) );
    Line.Begin( Build );
    if ( Line.EmitUnit( Unit ) != EEmitStatus::Ok or Line.Finish() != EEmitStatus::Ok )
    {
        OutError = "jit: this line did not emit: " + std::string( Line.Error() );
        return false;
    }

    if ( OutIr != nullptr )
    {
        // TakeModules moves the context out with them, which is exactly what is
        // wanted: the modules and the LLVMContext that types them die together
        // at the end of this scope, and nothing was ever added to a dylib.
        const Ir::OwnedModules Emitted = Ir::TakeModules( Line );

        // Only the modules that define a body. Under per-unit granularity most
        // of an emission is declaration — every unit whose code is already
        // resident — and printing those would bury the one module the question
        // was about.
        const auto DefinesABody = [] ( const llvm::Module &Mod )
        {
            for ( const llvm::Function &Fn : Mod )
            {
                if ( not Fn.isDeclaration() )
                {
                    return true;
                }
            }
            return false;
        };

        llvm::raw_string_ostream Text( *OutIr );
        for ( const std::unique_ptr<llvm::Module> &Mod : Emitted.Modules )
        {
            if ( Mod != nullptr and DefinesABody( *Mod ) )
            {
                Mod->print( Text, nullptr );
            }
        }
    }

    // No OpenGeneration, no OpenReplacement, no AddModules. The generator goes
    // out of scope here, abandoned, and the destruction order inside it is what
    // makes that safe (IrGeneratorState.hpp). That is the whole contract of a
    // probe: a question about a line costs the compilation and nothing else.
    return true;
}

std::string Volt::Backend::Jit::JitBackend::LastUnitIr () const
{
    return Impl->Compiler.LastIr();
}

void Volt::Backend::Jit::JitBackend::RecordIr ( const bool bEnable )
{
    Impl->Compiler.RecordIr( bEnable );
}

std::string Volt::Backend::Jit::JitBackend::Disassemble ( const std::uintptr_t Address, const std::size_t MaxBytes )
{
    std::string Ignored;
    return Impl->Compiler.Disassemble( Address, MaxBytes, Ignored );
}

std::size_t Volt::Backend::Jit::JitBackend::LiveGenerations () const
{
    return Impl->Compiler.LiveGenerations();
}

Volt::Backend::IJitBackend::BenchResult
Volt::Backend::Jit::JitBackend::BenchUnit ( const BackendInput &Build, const UnitView &Unit, const std::size_t Iterations )
{
    const auto Failed = [] ( std::string Why )
    { return BenchResult{ .bOk = false, .Message = std::move( Why ), .Iterations = 0, .TotalNanos = 0, .BestNanos = 0 }; };

    if ( not Impl->bMaterialised )
    {
        return Failed( "jit: the session was never materialised, so there is nothing to run against" );
    }
    if ( Iterations == 0 )
    {
        return Failed( "jit: a benchmark of zero iterations measures nothing" );
    }
    if ( not UnitHasInit( Unit ) )
    {
        return Failed( "jit: this unit has no top-level statements to run" );
    }

    Ir::IrGenerator Line( Impl->OneUnitOptions( Unit.Ordinal, /*bReplacing=*/false ) );
    Line.Begin( Build );
    if ( Line.EmitUnit( Unit ) != EEmitStatus::Ok or Line.Finish() != EEmitStatus::Ok )
    {
        return Failed( "jit: this line did not emit: " + std::string( Line.Error() ) );
    }
    if ( Line.UnwindStorageSize() > Impl->BootUnwindStorage )
    {
        return Failed( "repl: this line can raise a value wider than the session's unwind buffer" );
    }

    // A dylib of its own, always — not because the unit redefines anything,
    // but because this generation is going away, and a generation that shares
    // the main dylib takes its symbol table entries with it when it goes.
    GenerationId Gen = 0;
    std::string Error;
    if ( not Impl->Compiler.OpenReplacement( Gen, Error ) )
    {
        return Failed( std::move( Error ) );
    }

    // Everything below is on the path to DropGeneration, including every early
    // return: a benchmark that fails halfway must not leave a generation behind,
    // which is the one property `:bench` is asked to prove.
    const auto Drop = [&] ()
    {
        std::string Ignored;
        ( void )Impl->Compiler.DropGeneration( Gen, Ignored );
    };

    if ( not Impl->Compiler.AddModules( Gen, Ir::TakeModules( Line ), Error ) )
    {
        Drop();
        return Failed( std::move( Error ) );
    }

    // Nothing is recorded in Defined or Slotted. Those sets say what the
    // session *has*, and after the drop below it will have none of this — a
    // later line that reaches the same monomorphisation has to emit it again,
    // and would fail to resolve it if this claimed otherwise.

    const std::string InitSymbol = "_V_init_" + std::to_string( Unit.Ordinal );

    std::uintptr_t Address = 0;
    if ( not Impl->Compiler.LookupIn( Gen, InitSymbol, Address, Error ) )
    {
        Drop();
        return Failed( std::move( Error ) );
    }

    using InitFn       = void ( * )();
    const InitFn Body  = reinterpret_cast<InitFn>( Address ); // NOLINT(performance-no-int-to-ptr)
    std::uint32_t *Tag = Impl->ExceptionTag();

    BenchResult Out;
    Out.bOk        = true;
    Out.Iterations = Iterations;
    Out.BestNanos  = std::numeric_limits<std::uint64_t>::max();

    // One untimed call first. It pays for the lazy materialisation ORC does on
    // first lookup, which would otherwise land entirely in iteration one and
    // make every other number look like an improvement.
    if ( Tag != nullptr )
    {
        *Tag = UnwindTransport::NoExceptionTag;
    }
    Body();
    if ( Tag != nullptr and *Tag != UnwindTransport::NoExceptionTag )
    {
        *Tag = UnwindTransport::NoExceptionTag;
        Drop();
        return Failed( "repl: the benchmarked expression raised and never rescued" );
    }

    for ( std::size_t Round = 0; Round < Iterations; ++Round )
    {
        const std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
        Body();
        const std::chrono::steady_clock::time_point Ended = std::chrono::steady_clock::now();

        if ( Tag != nullptr and *Tag != UnwindTransport::NoExceptionTag )
        {
            *Tag = UnwindTransport::NoExceptionTag;
            Drop();
            return Failed( "repl: the benchmarked expression raised and never rescued" );
        }

        const auto Elapsed =
            static_cast<std::uint64_t>( std::chrono::duration_cast<std::chrono::nanoseconds>( Ended - Started ).count() );
        Out.TotalNanos += Elapsed;
        Out.BestNanos = std::min( Out.BestNanos, Elapsed );
    }

    Drop();
    return Out;
}
