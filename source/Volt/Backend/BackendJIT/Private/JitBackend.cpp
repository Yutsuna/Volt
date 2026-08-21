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
#include "Volt/Core/Support/PhaseTimer.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"
#include "Volt/BackendLlvmIr/LlvmAccess.hpp"

#include <cstdint>
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

    std::string Error;

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

    Ir::IrOptions Gen;
    Gen.Granularity = Ir::EModuleGranularity::Whole;
    Gen.Tls         = Ir::ETlsAccess::Accessor;
    Gen.Linkage     = Ir::ELinkage::Direct;

    // No inline-eligible exception to the skip: a skipped unit's code is in a
    // dylib and the JIT calls it there. The AOT path wants the opposite because
    // it can inline across the boundary; nothing here can.
    Gen.SkipUnitsBelow             = Impl->Options.SkipUnitsBelow;
    Gen.bDefineInlineEligibleBelow = false;

    Gen.TargetTriple       = Impl->Compiler.TargetTriple();
    Gen.DataLayout         = Impl->Compiler.DataLayoutString();
    Gen.bNeedTargetMachine = false;

    Gen.EntryFunction = Impl->Options.EntryFunction;
    Gen.EntrySymbol   = Impl->Options.EntrySymbol;
    Gen.bVerify       = true;

    Impl->Gen.emplace( std::move( Gen ) );
    Impl->Gen->Begin( Input );
}

Volt::Backend::EEmitStatus Volt::Backend::Jit::JitBackend::EmitUnit ( const UnitView &Unit )
{
    if ( Impl->Failed() or not Impl->Gen.has_value() )
    {
        return EEmitStatus::Error;
    }
    return Impl->Gen->EmitUnit( Unit );
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
    if ( not Impl->Compiler.AddModule( Impl->Generation, Ir::TakeModule( *Impl->Gen ), Error ) )
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

Volt::Backend::ReloadResult Volt::Backend::Jit::JitBackend::Reload ( const UnitView &Unit )
{
    // Needs PerUnit granularity and Indirect linkage, neither of which this
    // milestone emits. Refused rather than half-done: a reload that silently
    // did nothing would be worse than one that says it cannot.
    static_cast<void>( Unit );
    return ReloadResult{ .Status         = EReloadStatus::Refused,
                         .Message        = "jit: hot reload needs per-unit modules and indirect linkage",
                         .PatchedSymbols = 0 };
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
