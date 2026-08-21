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
#include "Functions/FunctionSlots.hpp"
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
    std::vector<IrGenerator::UnitSymbol> LastUnit;
    std::vector<IrGenerator::UnitShape> LastShapes;

    // --- Services ------------------------------------------------------------
    //
    // By pointer, and constructed in the body of State's constructor rather than
    // in an initialiser list, because each of them takes `Services` by reference
    // and `Services` has to name them all: the cycle is broken by handing every
    // service the bundle *after* the bundle's own storage exists.

    DiagnosticSink Diag;
    Llvm::ModuleContext Ctx;

    // Every module closed so far, oldest first. Empty in Whole granularity,
    // where the one module stays in Ctx until the emission is taken.
    //
    // Declared *after* Ctx, and that is not cosmetic: an llvm::Module's
    // destructor unregisters it from the LLVMContext that typed it, so a
    // module outliving its context dereferences freed memory. Members are
    // destroyed in reverse declaration order, so this has to come after the
    // context to be destroyed before it. The ordinary path never notices —
    // TakeModules empties this first — but a generator that is abandoned
    // rather than harvested (an emission refused before it is used) does.
    std::vector<std::unique_ptr<llvm::Module>> Closed;

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

    // Close the module being written and open the next one. The unit ordinal
    // is what tells the emitters which definitions belong here and which are
    // now somebody else's to hold; NoUnitOrdinal names the shared modules at
    // either end of the emission.
    void CloseModule ( const std::string &NextName, std::uint32_t NextUnit )
    {
        // Last thing this module gets: by now every body it will ever hold is
        // in it, so a slot can be initialised to its definition without any
        // call site having had to care which of the two was emitted first.
        Llvm::DefineLocalSlots( Services );

        if ( std::unique_ptr<llvm::Module> Finished = Ctx.Rotate( NextName ); Finished != nullptr )
        {
            Closed.push_back( std::move( Finished ) );
        }
        Services.CurrentUnit = NextUnit;
    }

    [[nodiscard]] bool Failed () const
    {
        return Diag.Failed();
    }

    EEmitStatus Fail ( std::string Message )
    {
        return Diag.Fail( std::move( Message ) );
    }
};
