#pragma once

// IrGeneratorState.hpp — IrGenerator's pimpl body.
//
// An *orchestrator*, nothing more: it owns the services, wires them into one
// EmitterServices bundle, and holds the build-wide caches that belong to no
// single service. Every emission method lives on whichever service actually
// holds the state it reads — TypeMapper, SignatureBuilder, FunctionRegistry,
// ExceptionLowering, ClosureLowering, MonoDriver — or on BodyEmitter, which is
// constructed per body over a frame the caller owns.
//
// Note what is *not* here: per-function state. That lives in FunctionFrame, a
// local of whoever is emitting a body, so "clear it at both ends" is enforced
// by scope rather than by someone remembering.
//
// Note also what is not here any more: the target tail. Verifying, optimising,
// writing a `.o` and driving a linker are BackendLLVM's, and running the module
// is BackendJIT's. This module stops at a finished llvm::Module.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"
#include "Volt/BackendCore/InstanceLayout.hpp"
#include "Volt/BackendCore/LayoutEngine.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Functions/FunctionRegistry.hpp"
#include "Functions/SignatureBuilder.hpp"
#include "Functions/VTableRegistry.hpp"
#include "Lower/Closure/ClosureLowering.hpp"
#include "Lower/Exception/ExceptionLowering.hpp"
#include "Lower/Mono/MonoDriver.hpp"
#include "Types/AbiVerifier.hpp"
#include "Types/TypeMapper.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct Volt::Backend::Ir::IrGenerator::State
{

    explicit State ( IrOptions InOptions );

    State ( const State & )           = delete;
    State &operator=( const State & ) = delete;

    // --- The middle-end's output, read-only except for layouts ---------------

    const BackendInput *Build = nullptr;
    IrOptions Options;
    std::optional<LayoutEngine> Layouts;
    InstanceLayouts Instances;

    // --- Build-wide caches that belong to no single service ------------------

    std::vector<std::uint8_t> PrecompiledUnits;
    Llvm::ModuleGlobalMap ModuleGlobals;
    Llvm::SynthesizedFnMap SynthesizedFns;

    // Symbols the last EmitUnit defined, for a hot-reload consumer that has to
    // know which indirection slots to repoint.
    std::vector<std::string> LastUnit;

    // --- Services ------------------------------------------------------------
    //
    // By pointer, and constructed in the body of State's constructor rather than
    // in an initialiser list, because each of them takes `Services` by reference
    // and `Services` has to name them all: the cycle is broken by handing every
    // service the bundle *after* the bundle's own storage exists.

    DiagnosticSink Diag;
    Llvm::ModuleContext Ctx;
    Llvm::EmitterServices Services;

    std::unique_ptr<Llvm::TypeMapper> Types;
    std::unique_ptr<Llvm::AbiVerifier> Abi;
    std::unique_ptr<Llvm::SignatureBuilder> Signatures;
    std::unique_ptr<Llvm::FunctionRegistry> Functions;
    std::unique_ptr<Llvm::ExceptionLowering> Exceptions;
    std::unique_ptr<Llvm::ClosureLowering> Closures;
    std::unique_ptr<Llvm::MonoDriver> Mono;
    std::unique_ptr<Llvm::VTableRegistry> VTables;

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
