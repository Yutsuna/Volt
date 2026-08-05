// LlvmBackend.cpp — the pimpl's construction and the class's trivial surface.
//
// State's constructor is the one place the service graph is wired: every service
// takes the EmitterServices bundle by reference, and the bundle names every
// service, so the cycle is broken by filling the bundle in *after* its own
// storage exists. Nothing else in the module knows how anything is constructed.

#include "Core/LlvmBackendState.hpp"

#include <memory>
#include <utility>

#ifndef DEBUG_NO_STATIC_ASSERT
    // The concept is the contract; breaking the signature is a compile error here,
    // not a discovery made at the Driver's runtime seam.
    static_assert( Volt::Backend::TargetBackend<Volt::Backend::Llvm::LlvmBackend> );
#endif

Volt::Backend::Llvm::LlvmBackend::State::State ()
{
    Services.Build          = nullptr;
    Services.Options        = &Options;
    Services.Layouts        = &Layouts;
    Services.Instances      = &Instances;
    Services.Diag           = &Diag;
    Services.Ctx            = &Ctx;
    Services.ModuleGlobals  = &ModuleGlobals;
    Services.SynthesizedFns = &SynthesizedFns;

    Types      = std::make_unique<TypeMapper>( Services );
    Abi        = std::make_unique<AbiVerifier>( Services );
    Signatures = std::make_unique<SignatureBuilder>( Services );
    Functions  = std::make_unique<FunctionRegistry>( Services );
    Exceptions = std::make_unique<ExceptionLowering>( Services );
    Closures   = std::make_unique<ClosureLowering>( Services );
    Mono       = std::make_unique<MonoDriver>( Services );
    Pipeline   = std::make_unique<TargetPipeline>( Services );
    Linker     = std::make_unique<LinkerDriver>( Services );

    Services.Types      = Types.get();
    Services.Abi        = Abi.get();
    Services.Signatures = Signatures.get();
    Services.Functions  = Functions.get();
    Services.Exceptions = Exceptions.get();
    Services.Closures   = Closures.get();
    Services.Mono       = Mono.get();
    Services.Linker     = Linker.get();
}

Volt::Backend::Llvm::LlvmBackend::LlvmBackend () : Impl( std::make_unique<State>() )
{
}

Volt::Backend::Llvm::LlvmBackend::~LlvmBackend () = default;

Volt::Backend::Llvm::LlvmBackend::LlvmBackend ( LlvmBackend && ) noexcept = default;

Volt::Backend::Llvm::LlvmBackend &Volt::Backend::Llvm::LlvmBackend::operator=( LlvmBackend && ) noexcept = default;

void Volt::Backend::Llvm::LlvmBackend::SetOptions ( EmitOptions InOptions )
{
    Impl->Options = std::move( InOptions );
}
