// LlvmEmitter.cpp — LlvmBackend's lifecycle and the two sweeps over the units.
//
// The emission itself lives in sibling TUs (TypeMapper, ExprEmitter, ...);
// this file owns only the shape of a build: set up the module and the host
// target, declare everything reachable, then define it.

#include "LlvmState.hpp"

#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <mutex>
#include <utility>

// The concept is the contract; breaking the signature is a compile error here,
// not a discovery made at the Driver's runtime seam.
static_assert( Volt::Backend::TargetBackend<Volt::Backend::Llvm::LlvmBackend> );

namespace
{

// LLVM's target registry is process-global and not re-entrant. Volt compiles
// units in parallel elsewhere, so the initialisation is guarded even though
// codegen itself is single-threaded.
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

} // namespace

Volt::Backend::EEmitStatus Volt::Backend::Llvm::LlvmBackend::State::Fail ( std::string InMessage )
{
    if ( Status != EEmitStatus::Error )
    {
        Status  = EEmitStatus::Error;
        Message = std::move( InMessage );
    }
    return EEmitStatus::Error;
}

bool Volt::Backend::Llvm::LlvmBackend::State::InitTarget ( std::string_view ModuleName )
{
    InitialiseNativeTarget();

    Mod     = std::make_unique<llvm::Module>( ModuleName, Context );
    Builder = std::make_unique<llvm::IRBuilder<>>( Context );

    // One string is the whole cross-compilation seam.
    const std::string TripleText = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple Triple{ TripleText };
    Mod->setTargetTriple( Triple );

    std::string Error;
    const llvm::Target *Target = llvm::TargetRegistry::lookupTarget( Triple, Error );
    if ( Target == nullptr )
    {
        static_cast<void>( Fail( "llvm: no target for host triple '" + TripleText + "': " + Error ) );
        return false;
    }

    Machine.reset( Target->createTargetMachine( Triple, "generic", "", llvm::TargetOptions{}, std::nullopt ) );
    if ( Machine == nullptr )
    {
        static_cast<void>( Fail( "llvm: could not create a TargetMachine for '" + TripleText + "'" ) );
        return false;
    }

    // Set before any type is created: the ABI cross-check in TypeMapper reads
    // struct offsets out of this DataLayout and compares them against
    // LayoutEngine, so an unset layout would make the check vacuous.
    Mod->setDataLayout( Machine->createDataLayout() );
    return true;
}

Volt::Backend::Llvm::LlvmBackend::LlvmBackend () : Impl( std::make_unique<State>() )
{
}

Volt::Backend::Llvm::LlvmBackend::~LlvmBackend () = default;

Volt::Backend::Llvm::LlvmBackend::LlvmBackend ( LlvmBackend && ) noexcept = default;

Volt::Backend::Llvm::LlvmBackend &Volt::Backend::Llvm::LlvmBackend::operator=( LlvmBackend && ) noexcept = default;

void Volt::Backend::Llvm::LlvmBackend::Begin ( const BackendInput &Input )
{
    Impl->Build = &Input;

    if ( Input.Types == nullptr )
    {
        static_cast<void>( Impl->Fail( "llvm: the build carries no TypeStore" ) );
        return;
    }

    Impl->Layouts.emplace( *Input.Types );

    // One llvm::Module per build: the simplest correct thing. Per-unit modules
    // plus ThinLTO is a later optimisation behind this same interface. The
    // entry module is last in circuit link order, so it names the module.
    const std::string_view Name = Input.Units.empty() ? std::string_view{ "volt" } : Input.Units.back().Module;
    static_cast<void>( Impl->InitTarget( Name ) );
}

Volt::Backend::EEmitStatus Volt::Backend::Llvm::LlvmBackend::EmitUnit ( [[maybe_unused]] const UnitView &Unit )
{
    if ( Impl->Failed() )
    {
        return EEmitStatus::Error;
    }
    return EEmitStatus::Unimplemented;
}

Volt::Backend::EmitResult Volt::Backend::Llvm::LlvmBackend::Finalize ()
{
    if ( Impl->Failed() )
    {
        return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Impl->Message };
    }

    return EmitResult{
        .Status = EEmitStatus::Unimplemented, .Artifact = {}, .Message = "llvm backend: emission not implemented yet" };
}
