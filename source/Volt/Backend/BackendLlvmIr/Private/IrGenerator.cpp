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

#include "Functions/FunctionSlots.hpp"

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
    //
    // PerUnit opens on a module of its own all the same, because DeclareAll
    // below has to write its declarations somewhere and no unit owns them. It
    // ends up holding nothing but declarations, and a consumer is free to drop
    // it for that reason.
    const bool bPerUnit         = Impl->Options.Granularity == EModuleGranularity::PerUnit;
    const std::string_view Name = bPerUnit              ? std::string_view{ "volt.declarations" }
                                  : Input.Units.empty() ? std::string_view{ "volt" }
                                                        : Input.Units.back().Module;

    const Llvm::TargetSpec Spec{ .Triple             = Impl->Options.TargetTriple,
                                 .DataLayout         = Impl->Options.DataLayout,
                                 .bNeedTargetMachine = Impl->Options.bNeedTargetMachine };

    std::string Error;
    if ( not Impl->Ctx.InitTarget( Name, Spec, Error ) )
    {
        static_cast<void>( Impl->Fail( std::move( Error ) ) );
        return;
    }

    // Before DeclareAll, because the linkage every declaration gets depends on
    // whether its unit will be defined here.
    Impl->PrecompiledUnits          = Llvm::ResolvePrecompiledUnits( Impl->Services );
    Impl->Services.PrecompiledUnits = &Impl->PrecompiledUnits;

    // Declare before defining anything: a body emitted in the first unit may call
    // something declared in the last, and one pass over the store means that
    // resolves immediately instead of needing a fixup pass.
    Llvm::DeclareAll( Impl->Services );
}

namespace
{

// Every nominal the unit declares, with the size and alignment its instances
// actually have. Read straight after the unit is emitted, when the layout
// engine has resolved everything the unit's own bodies needed.
std::vector<Volt::Backend::Ir::IrGenerator::UnitShape> CollectShapes ( Volt::Backend::Ir::IrGenerator::State &Impl,
                                                                       std::uint32_t Ordinal )
{
    std::vector<Volt::Backend::Ir::IrGenerator::UnitShape> Out;
    if ( Impl.Build == nullptr or Impl.Build->Types == nullptr or not Impl.Layouts.has_value() )
    {
        return Out;
    }

    const Volt::MiddleEnd::TypeSystem::TypeStore &Store = *Impl.Build->Types;
    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        using NominalId = Volt::MiddleEnd::TypeSystem::NominalId;
        const NominalId Id{ static_cast<NominalId::ValueType>( Index ) };
        const Volt::MiddleEnd::TypeSystem::NominalType &Type = Store.Type( Id );
        if ( Type.Unit != Ordinal or not Type.Layout.IsValid() )
        {
            continue;
        }
        const Volt::Backend::SizeAlign Shape = Impl.Layouts->Of( Type.Layout );
        Out.push_back( { .Name = std::string( Store.Text( Type.Name ) ), .Size = Shape.Size, .Alignment = Shape.Alignment } );
    }
    return Out;
}

} // namespace

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

    // Everything below this point belongs to this unit's own module. Opened
    // before the precompiled branch, not after it: a skipped unit still emits
    // its lifted closures, and those are its own to hold too.
    if ( Impl->Options.Granularity == EModuleGranularity::PerUnit )
    {
        Impl->CloseModule( "volt.unit." + std::to_string( Unit.Ordinal ), Unit.Ordinal );
    }

    if ( bPrecompiled and not Impl->Options.bDefineInlineEligibleBelow )
    {
        // Its ordinary bodies come from the artifact, but its lifted closure
        // bodies cannot: they carry no mangled symbol, so nothing can import
        // them, and a generic from this unit instantiated *here* reaches one
        // through a FuncAddr.
        Llvm::DefineSynthesizedOnly( Impl->Services, Unit );
        return Impl->Failed() ? EEmitStatus::Error : EEmitStatus::Ok;
    }

    {
        const Volt::Core::PhaseScope Timing( "backend.emit" );
        Llvm::DefineAll( Impl->Services, Unit, /*bInlineEligibleOnly=*/bPrecompiled );
    }

    // Read while this unit's module is still the open one — under PerUnit that
    // module holds this unit's definitions and nothing else, which is exactly
    // the set a reload of this unit would have to repoint.
    Impl->LastUnit   = Llvm::LocalDefinedSymbols( Impl->Services );
    Impl->LastShapes = CollectShapes( *Impl, Unit.Ordinal );
    return Impl->Failed() ? EEmitStatus::Error : EEmitStatus::Ok;
}

Volt::Backend::EEmitStatus Volt::Backend::Ir::IrGenerator::Finish ()
{
    if ( Impl->Failed() )
    {
        return EEmitStatus::Error;
    }

    // What Finish emits belongs to no unit: an instantiation is minted for the
    // build, `_V_init_all` names every unit at once, and the entry point is the
    // build's. Under PerUnit they go to a shared module of their own, so no
    // unit's module can be reloaded out from under them.
    if ( Impl->Options.Granularity == EModuleGranularity::PerUnit )
    {
        Impl->CloseModule( "volt.shared", Llvm::EmitterServices::NoUnitOrdinal );
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
    // The module Finish itself filled never goes through CloseModule, so its
    // own definitions — the instantiations, and the seams — get their slots
    // here instead.
    Llvm::DefineLocalSlots( Impl->Services );

    // ... and, for the same reason, are recorded here rather than there. This
    // is the module the monomorphisations land in, so a consumer that emits
    // again into the same live program would be told this build defines none of
    // them and would emit a second copy of every one.
    for ( std::string &Name : Llvm::LocalDefinedNames( Impl->Services ) )
    {
        Impl->AllDefined.push_back( std::move( Name ) );
    }

    if ( Impl->Options.bVerify )
    {
        const Volt::Core::PhaseScope Timing( "backend.verify" );

        // Every module, not only the one still open. ORC materialises lazily,
        // so a module nothing looks up is never code-generated and its
        // malformed IR would surface as nothing at all — or as a crash the
        // first time some later run did reach it.
        const auto Verify = [this] ( llvm::Module &Mod ) -> bool
        {
            std::string Text;
            llvm::raw_string_ostream Stream( Text );
            if ( not llvm::verifyModule( Mod, &Stream ) )
            {
                return true;
            }
            for ( const llvm::Function &Fn : Mod )
            {
                if ( llvm::verifyFunction( Fn ) )
                {
                    static_cast<void>( Impl->Fail( "llvm: module verification failed in '" + std::string( Fn.getName() ) +
                                                   "': " + Stream.str() ) );
                    return false;
                }
            }
            static_cast<void>( Impl->Fail( "llvm: module verification failed: " + Stream.str() ) );
            return false;
        };

        for ( const std::unique_ptr<llvm::Module> &Mod : Impl->Closed )
        {
            if ( Mod != nullptr and not Verify( *Mod ) )
            {
                return EEmitStatus::Error;
            }
        }
        if ( not Verify( Impl->Ctx.Mod() ) )
        {
            return EEmitStatus::Error;
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

std::string Volt::Backend::Ir::SlotNameOf ( std::string_view Symbol )
{
    return "volt.fn." + std::string( Symbol );
}

std::vector<Volt::Backend::Ir::IrGenerator::UnitSymbol> Volt::Backend::Ir::IrGenerator::LastUnitSymbols () const
{
    return Impl->LastUnit;
}

std::vector<std::string> Volt::Backend::Ir::IrGenerator::DefinedSymbols () const
{
    return Impl->AllDefined;
}

std::vector<Volt::Backend::Ir::IrGenerator::VTableEntry> Volt::Backend::Ir::IrGenerator::VTableEntries () const
{
    return Impl->VTables == nullptr ? std::vector<VTableEntry>{} : Impl->VTables->Entries();
}

std::vector<Volt::Backend::Ir::IrGenerator::UnitShape> Volt::Backend::Ir::IrGenerator::LastUnitShapes () const
{
    return Impl->LastShapes;
}

// --- LlvmAccess.hpp ---------------------------------------------------------
//
// Defined here rather than in a file of its own: they are four one-line reaches
// into State, and State is only complete in this translation unit.

Volt::Backend::Ir::OwnedModules Volt::Backend::Ir::TakeModules ( IrGenerator &Gen )
{
    IrGenerator::State *Impl = Gen.Peek();

    // Modules first: the one still open is emptied along with the builder, and
    // the context has to outlive all of them.
    OwnedModules Out;
    Out.Modules = std::move( Impl->Closed );
    Impl->Closed.clear();

    if ( std::unique_ptr<llvm::Module> Last = Impl->Ctx.TakeModule(); Last != nullptr )
    {
        Out.Modules.push_back( std::move( Last ) );
    }
    Out.Context = Impl->Ctx.TakeContext();
    return Out;
}

llvm::Module &Volt::Backend::Ir::ModuleOf ( IrGenerator &Gen )
{
    return Gen.Peek()->Ctx.Mod();
}

llvm::TargetMachine *Volt::Backend::Ir::MachineOf ( IrGenerator &Gen )
{
    return Gen.Peek()->Ctx.MachinePtr();
}
