#pragma once

// LlvmBackendState.hpp — LlvmBackend's pimpl body.
//
// This used to be a 734-line god object: every LLVM handle, every cache, every
// per-function scratch field, and ~90 emission methods declared on one class.
// What is left is an *orchestrator*: it owns the services, wires them into one
// EmitterServices bundle, and does nothing else. Every method that used to live
// here now belongs to whichever service actually holds the state it reads —
// TypeMapper, SignatureBuilder, FunctionRegistry, ExceptionLowering,
// ClosureLowering, MonoDriver, TargetPipeline, LinkerDriver — or to BodyEmitter,
// which is constructed per body over a frame the caller owns.
//
// Note what is *not* here any more: `FunctionFrame Frame`. Per-function state
// being a member of a long-lived object is what made "clear it at both ends of
// every body" a rule someone had to remember; it is a local now
// (Lower/FunctionFrame.hpp), so the rule is enforced by scope instead.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/InstanceLayout.hpp"
#include "Volt/BackendCore/LayoutEngine.hpp"
#include "Volt/BackendLLVM/LlvmEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Functions/FunctionRegistry.hpp"
#include "Functions/SignatureBuilder.hpp"
#include "Functions/VTableRegistry.hpp"
#include "Lower/Closure/ClosureLowering.hpp"
#include "Lower/Exception/ExceptionLowering.hpp"
#include "Lower/Mono/MonoDriver.hpp"
#include "Target/LinkerDriver.hpp"
#include "Target/TargetPipeline.hpp"
#include "Types/AbiVerifier.hpp"
#include "Types/TypeMapper.hpp"

#include <memory>
#include <optional>

struct Volt::Backend::Llvm::LlvmBackend::State
{

    State ();

    State ( const State & )           = delete;
    State &operator=( const State & ) = delete;

    // --- The middle-end's output, read-only except for layouts ---------------

    const BackendInput *Build = nullptr;
    EmitOptions Options;
    std::optional<LayoutEngine> Layouts;
    InstanceLayouts Instances;

    // --- Build-wide caches that belong to no single service ------------------

    ModuleGlobalMap ModuleGlobals;
    SynthesizedFnMap SynthesizedFns;

    // --- Services ------------------------------------------------------------
    //
    // By pointer, and constructed in the body of State's constructor rather than
    // in an initialiser list, because each of them takes `Services` by reference
    // and `Services` has to name them all: the cycle is broken by handing every
    // service the bundle *after* the bundle's own storage exists.

    DiagnosticSink Diag;
    ModuleContext Ctx;
    EmitterServices Services;

    std::unique_ptr<TypeMapper> Types;
    std::unique_ptr<AbiVerifier> Abi;
    std::unique_ptr<SignatureBuilder> Signatures;
    std::unique_ptr<FunctionRegistry> Functions;
    std::unique_ptr<ExceptionLowering> Exceptions;
    std::unique_ptr<ClosureLowering> Closures;
    std::unique_ptr<MonoDriver> Mono;
    std::unique_ptr<TargetPipeline> Pipeline;
    std::unique_ptr<LinkerDriver> Linker;
    std::unique_ptr<VTableRegistry> VTables;

    // --- Convenience ---------------------------------------------------------

    [[nodiscard]] bool Failed () const
    {
        return Diag.Failed();
    }

    EEmitStatus Fail ( std::string Message )
    {
        return Diag.Fail( std::move( Message ) );
    }
};
