#include "Volt/BackendVM/VmBackend.hpp"

#include "Volt/BackendVM/VirtualMachine.hpp"

// The concept is the contract; breaking the signature is a compile error
// here, not a discovery made at the Driver's runtime seam.
static_assert( Volt::Backend::TargetBackend<Volt::Backend::VM::VmBackend> );

void Volt::Backend::VM::VmBackend::Begin ( const BackendInput &Input )
{
    Build = &Input;
    Functions.Chunks.clear();
}

Volt::Backend::EEmitStatus Volt::Backend::VM::VmBackend::EmitUnit ( [[maybe_unused]] const UnitView &Unit )
{
    return EEmitStatus::Unimplemented;
}

Volt::Backend::EmitResult Volt::Backend::VM::VmBackend::Finalize ()
{
    return EmitResult{
        .Status = EEmitStatus::Unimplemented, .Artifact = {}, .Message = "vm backend: emission not implemented yet" };
}

Volt::Backend::VM::ERunStatus Volt::Backend::VM::VirtualMachine::Run ( [[maybe_unused]] const FunctionTable &Functions,
                                                                       [[maybe_unused]] std::size_t Entry )
{
    return ERunStatus::Unimplemented;
}
