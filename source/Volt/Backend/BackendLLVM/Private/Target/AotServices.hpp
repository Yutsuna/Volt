#pragma once

// AotServices.hpp — what the ahead-of-time tail needs, and nothing more.
//
// The four steps below this header — verify, optimise, `.ll`, `.o` — and the
// linker driver used to be handed BackendLlvmIr's EmitterServices, the same bag
// every expression emitter gets. They never wanted it: between them they read
// five things, and reaching for the emitter bundle would mean this module
// includes BackendLlvmIr's *private* headers, which is exactly the coupling the
// split exists to remove. BackendJIT will have its own equivalent of this
// struct, and the two will share nothing but the module they point at — which
// is the point.
//
// A bag of pointers, not an owner: LlvmBackend::State owns, this refers.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"
#include "Volt/BackendLLVM/LlvmEmitter.hpp"

namespace llvm
{

class Module;
class TargetMachine;

} // namespace llvm

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        struct AotServices
        {

            // The middle-end's output, for the one thing the tail reads out of
            // it: the @[External] libraries the link has to name.
            const BackendInput *Build = nullptr;

            const EmitOptions *Options = nullptr;
            DiagnosticSink *Diag       = nullptr;

            // Borrowed from the IrGenerator, which still owns them. Valid for
            // exactly as long as the generator is alive and has not been asked
            // to hand its module over.
            llvm::Module *Mod            = nullptr;
            llvm::TargetMachine *Machine = nullptr;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
