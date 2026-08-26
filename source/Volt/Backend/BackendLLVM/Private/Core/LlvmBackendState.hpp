#pragma once

// LlvmBackendState.hpp — LlvmBackend's pimpl body.
//
// What is left of this module after the split: an ahead-of-time *tail*. The
// emission itself — every type, signature, body, closure and rescue — belongs
// to BackendLlvmIr, reached through one `IrGenerator` below. This object owns
// the options `volt build` was invoked with, the generator, and the two
// services that turn a finished module into a file on disk.
//
// The generator is an optional rather than a member, because SkipUnitsBelow is
// not known until Begin sees the build's StdlibUnitCount: the options are a
// constructor argument by design, so the construction waits for the facts.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"
#include "Volt/BackendLLVM/LlvmEmitter.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"

#include "Target/AotServices.hpp"
#include "Target/LinkerDriver.hpp"
#include "Target/TargetPipeline.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

struct Volt::Backend::Llvm::LlvmBackend::State
{

    State () = default;

    State ( const State & )           = delete;
    State &operator=( const State & ) = delete;

    const BackendInput *Build = nullptr;
    EmitOptions Options;

    // Failures raised by the tail itself — a link that would not run, an
    // unwritable output. Emission failures live in the generator's own sink and
    // are read back through Error().
    DiagnosticSink Diag;

    std::optional<Ir::IrGenerator> Gen;

    AotServices Services;
    std::unique_ptr<TargetPipeline> Pipeline;
    std::unique_ptr<LinkerDriver> Linker;

    [[nodiscard]] bool Failed () const
    {
        return Diag.Failed() or ( Gen.has_value() and Gen->Failed() );
    }

    // Whichever half failed. The tail's own sink wins when both did, because it
    // failed later and its message is the one describing what the caller asked
    // for.
    [[nodiscard]] std::string Message () const
    {
        if ( Diag.Failed() )
        {
            return Diag.Message();
        }
        return Gen.has_value() ? std::string( Gen->Error() ) : std::string{};
    }

    EEmitStatus Fail ( std::string InMessage )
    {
        return Diag.Fail( std::move( InMessage ) );
    }
};
