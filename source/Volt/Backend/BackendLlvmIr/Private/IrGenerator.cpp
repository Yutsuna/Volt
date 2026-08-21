// IrGenerator.cpp — Begin / EmitUnit / Finish, and the service graph.
//
// The shape of an emission, and nothing else: set up the module and its target,
// declare everything reachable, define it unit by unit, then drain the
// monomorphiser and cap the module with its entry point. Every step delegates;
// the *ordering* is the content of this file, and it is load-bearing at three
// points in particular —
//
//   - DeclareAll runs before any body, so a body in the first unit calling
//     something declared in the last resolves with no fixup pass;
//   - the monomorphiser drains before the entry point is emitted, so the entry
//     point can itself be the thing that forced an instantiation;
//   - verification, when asked for, runs after that, so the shim is proved
//     well-formed like any other function.
//
// State's constructor is the one place the service graph is wired: every
// service takes the EmitterServices bundle by reference, and the bundle names
// every service, so the cycle is broken by filling the bundle in *after* its own
// storage exists. Nothing else in the module knows how anything is constructed.

#include "IrGeneratorState.hpp"

#include "Volt/BackendLlvmIr/LlvmAccess.hpp"

#include "Volt/Core/Support/PhaseTimer.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

Volt::Backend::Ir::IrGenerator::State::State ( IrOptions InOptions ) : Options( std::move( InOptions ) )
{
    Services.Build          = nullptr;
    Services.Options        = &Options;
    Services.Layouts        = &Layouts;
    Services.Instances      = &Instances;
    Services.Diag           = &Diag;
    Services.Ctx            = &Ctx;
    Services.ModuleGlobals  = &ModuleGlobals;
    Services.SynthesizedFns = &SynthesizedFns;

    Types      = std::make_unique<Llvm::TypeMapper>( Services );
    Abi        = std::make_unique<Llvm::AbiVerifier>( Services );
    Signatures = std::make_unique<Llvm::SignatureBuilder>( Services );
    Functions  = std::make_unique<Llvm::FunctionRegistry>( Services );
    Exceptions = std::make_unique<Llvm::ExceptionLowering>( Services );
    Closures   = std::make_unique<Llvm::ClosureLowering>( Services );
    Mono       = std::make_unique<Llvm::MonoDriver>( Services );
    VTables    = std::make_unique<Llvm::VTableRegistry>( Services );

    Services.Types      = Types.get();
    Services.Abi        = Abi.get();
    Services.Signatures = Signatures.get();
    Services.Functions  = Functions.get();
    Services.Exceptions = Exceptions.get();
    Services.Closures   = Closures.get();
    Services.Mono       = Mono.get();
    Services.VTables    = VTables.get();
}

Volt::Backend::Ir::IrGenerator::IrGenerator ( IrOptions InOptions ) : Impl( std::make_unique<State>( std::move( InOptions ) ) )
{
}

Volt::Backend::Ir::IrGenerator::~IrGenerator () = default;

Volt::Backend::Ir::IrGenerator::IrGenerator ( IrGenerator && ) noexcept = default;

Volt::Backend::Ir::IrGenerator &Volt::Backend::Ir::IrGenerator::operator=( IrGenerator && ) noexcept = default;

void Volt::Backend::Ir::IrGenerator::Begin ( const BackendInput &Input )
{
    Impl->Build          = &Input;
    Impl->Services.Build = &Input;

    if ( Input.Types == nullptr )
    {
        static_cast<void>( Impl->Fail( "llvm: the build carries no TypeStore" ) );
        return;
    }

    Impl->Layouts.emplace( *Input.Types );

    // One llvm::Module per build, in Whole granularity: the simplest correct
    // thing. The entry module is last in circuit link order, so it names the
    // module.
    const std::string_view Name = Input.Units.empty() ? std::string_view{ "volt" } : Input.Units.back().Module;

    const Llvm::TargetSpec Spec{ .Triple             = Impl->Options.TargetTriple,
                                 .DataLayout         = Impl->Options.DataLayout,
                                 .bNeedTargetMachine = Impl->Options.bNeedTargetMachine };

    std::string Error;
    if ( not Impl->Ctx.InitTarget( Name, Spec, Error ) )
    {
        static_cast<void>( Impl->Fail( std::move( Error ) ) );
        return;
    }

    // Declare before defining anything: a body emitted in the first unit may call
    // something declared in the last, and one pass over the store means that
    // resolves immediately instead of needing a fixup pass.
    Llvm::DeclareAll( Impl->Services );
}

Volt::Backend::EEmitStatus Volt::Backend::Ir::IrGenerator::EmitUnit ( const UnitView &Unit )
{
    if ( Impl->Failed() )
    {
        return EEmitStatus::Error;
    }

    if ( Impl->Build == nullptr or Impl->Build->Types == nullptr )
    {
        return Impl->Fail( "llvm: unit '" + std::string( Unit.Path ) +
                           "' reached the backend with no build input or type store" );
    }

    if ( Unit.Ast == nullptr or Unit.Values == nullptr or Unit.Callees == nullptr or Unit.Scopes == nullptr )
    {
        return Impl->Fail( "llvm: unit '" + std::string( Unit.Path ) + "' reached the backend with no sema output" );
    }

    // A unit below the skip line already exists as compiled code somewhere else
    // — a precompiled stdlib artifact. It is declared (Begin did that for the
    // whole build) but not defined, except that the AOT tail still wants the
    // inline-eligible bodies so an optimised build can inline across the
    // boundary. The JIT wants the skip without that exception, which is why the
    // two are separate options rather than one.
    const bool bPrecompiled = Llvm::UnitIsPrecompiled( Impl->Services, Unit.Ordinal );

    if ( bPrecompiled and not Impl->Options.bDefineInlineEligibleBelow )
    {
        return EEmitStatus::Ok;
    }

    {
        const Volt::Core::PhaseScope Timing( "backend.emit" );
        Llvm::DefineAll( Impl->Services, Unit, /*bInlineEligibleOnly=*/bPrecompiled );
    }
    return Impl->Failed() ? EEmitStatus::Error : EEmitStatus::Ok;
}

Volt::Backend::EEmitStatus Volt::Backend::Ir::IrGenerator::Finish ()
{
    if ( Impl->Failed() )
    {
        return EEmitStatus::Error;
    }

    // Every unit is defined by now, so every instantiation a concrete body could
    // ever discover has been enqueued. A drained body can itself enqueue more — a
    // generic method calling another generic method — so this drains to a
    // fixpoint rather than once.
    {
        const Volt::Core::PhaseScope Timing( "backend.monomorphize" );
        Impl->Mono->Drain();
    }
    if ( Impl->Failed() )
    {
        return EEmitStatus::Error;
    }

    // After the drain, so the entry point can itself be the thing that forced an
    // instantiation, and before any verification, which is what proves the shim
    // is well formed like any other function.
    if ( not Llvm::EmitEntryPoint( Impl->Services ) )
    {
        return EEmitStatus::Error;
    }

    // After the entry point, so the slots it may itself have created exist by
    // now and the accessor points at the same ones every body uses.
    if ( Impl->Options.bDefineSlotAccessor and not Impl->Exceptions->EmitSlotAccessor() )
    {
        return EEmitStatus::Error;
    }

    // Off by default: BackendLLVM verifies in its own pipeline, where it can
    // name the offending function. A consumer with no such pipeline — the JIT —
    // turns this on, because handing a malformed module to ORC crashes inside
    // the JIT instead of reporting anything.
    if ( Impl->Options.bVerify )
    {
        const Volt::Core::PhaseScope Timing( "backend.verify" );
        std::string Text;
        llvm::raw_string_ostream Stream( Text );
        if ( llvm::verifyModule( Impl->Ctx.Mod(), &Stream ) )
        {
            for ( const llvm::Function &Fn : Impl->Ctx.Mod() )
            {
                if ( llvm::verifyFunction( Fn ) )
                {
                    return Impl->Fail( "llvm: module verification failed in '" + std::string( Fn.getName() ) +
                                       "': " + Stream.str() );
                }
            }
            return Impl->Fail( "llvm: module verification failed: " + Stream.str() );
        }
    }

    return EEmitStatus::Ok;
}

std::size_t Volt::Backend::Ir::IrGenerator::UnwindStorageSize () const
{
    return Impl->Exceptions->StorageSize();
}

bool Volt::Backend::Ir::IrGenerator::Failed () const noexcept
{
    return Impl->Failed();
}

std::string_view Volt::Backend::Ir::IrGenerator::Error () const noexcept
{
    return Impl->Diag.Message();
}

std::vector<std::string> Volt::Backend::Ir::IrGenerator::LastUnitSymbols () const
{
    return Impl->LastUnit;
}

// --- LlvmAccess.hpp ---------------------------------------------------------
//
// Defined here rather than in a file of its own: they are four one-line reaches
// into State, and State is only complete in this translation unit.

Volt::Backend::Ir::OwnedModule Volt::Backend::Ir::TakeModule ( IrGenerator &Gen )
{
    IrGenerator::State *Impl = Gen.Peek();

    // Module first: it is emptied along with the builder, and the context has
    // to outlive both.
    std::unique_ptr<llvm::Module> Mod = Impl->Ctx.TakeModule();
    return OwnedModule{ .Context = Impl->Ctx.TakeContext(), .Module = std::move( Mod ) };
}

std::vector<Volt::Backend::Ir::OwnedModule> Volt::Backend::Ir::TakeUnitModules ( IrGenerator &Gen )
{
    // Whole granularity keeps everything in the one module TakeModule hands
    // back. PerUnit lands with the hot-reload milestone.
    static_cast<void>( Gen );
    return {};
}

llvm::Module &Volt::Backend::Ir::ModuleOf ( IrGenerator &Gen )
{
    return Gen.Peek()->Ctx.Mod();
}

llvm::TargetMachine *Volt::Backend::Ir::MachineOf ( IrGenerator &Gen )
{
    return Gen.Peek()->Ctx.MachinePtr();
}
