#include "Volt/BackendLLVM/LlvmEmitter.hpp"

// The stable C API is enough for the skeleton: it proves include paths and
// linkage without dragging the C++ headers into every rebuild.
#include <llvm-c/Core.h>

static_assert( Volt::Backend::TargetBackend<Volt::Backend::Llvm::LlvmBackend> );

namespace Volt::Backend::Llvm
{

struct LlvmBackend::State
{

    LLVMContextRef Context = nullptr;

    State () : Context( LLVMContextCreate() )
    {
    }

    ~State ()
    {
        LLVMContextDispose( Context );
    }

    State ( const State & )            = delete;
    State &operator= ( const State & ) = delete;
};

} // namespace Volt::Backend::Llvm

Volt::Backend::Llvm::LlvmBackend::LlvmBackend () : Impl( std::make_unique<State>() )
{
}

Volt::Backend::Llvm::LlvmBackend::~LlvmBackend () = default;

Volt::Backend::Llvm::LlvmBackend::LlvmBackend ( LlvmBackend && ) noexcept = default;

Volt::Backend::Llvm::LlvmBackend &Volt::Backend::Llvm::LlvmBackend::operator= ( LlvmBackend && ) noexcept = default;

void Volt::Backend::Llvm::LlvmBackend::Begin ( const BackendInput & )
{
}

Volt::Backend::EEmitStatus Volt::Backend::Llvm::LlvmBackend::EmitUnit ( const UnitView & )
{
    return EEmitStatus::Unimplemented;
}

Volt::Backend::EmitResult Volt::Backend::Llvm::LlvmBackend::Finalize ()
{
    return EmitResult{ .Status = EEmitStatus::Unimplemented, .Artifact = {}, .Message = "llvm backend: emission not implemented yet" };
}
