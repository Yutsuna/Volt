#pragma once

// TargetPipeline.hpp — verify, optimise, emit.
//
// Everything here runs once, in Finalize(), after every unit is defined and the
// monomorphiser has drained to a fixpoint: the module is complete and never
// grows again past this point. The four steps are separate files because they
// are separate concerns with separate LLVM dependencies (the verifier, the pass
// builder, the IR printer, the target's own object emitter); this class is the
// seam that owns them together.

#include "Core/EmitterServices.hpp"

#include <string_view>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class TargetPipeline
        {

        public:

            explicit TargetPipeline ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            // llvm::verifyModule over the finished module. A failure here is an
            // emitter bug, never a Volt source error (rules/core-ast.md's
            // contract means the module the middle-end describes is always
            // well-formed), so it is reported with the offending function named,
            // not guessed at.
            [[nodiscard]] bool VerifyModule ();

            // PassBuilder's own default pipelines, chosen by Options.OptLevel /
            // Options.bLto: O0 for a dev build (buildO0DefaultPipeline — the
            // minimal semantically-required set, principally mem2reg, since the
            // emitter relies on it to turn its allocas back into registers),
            // O2/O3 otherwise. `bLto` selects O3 rather than a real cross-module
            // LTO run: one llvm::Module already holds the whole build (llvm.md),
            // so there is nothing to link across yet.
            void RunOptimizationPipeline () const;

            // Write the module as textual IR to Path — the `--emit ir` artifact.
            [[nodiscard]] bool EmitIrFile ( std::string_view Path );

            // TargetMachine::addPassesToEmitFile — the `--emit obj` artifact, and
            // the input LinkerDriver hands to the system linker when `--emit` is
            // unset.
            [[nodiscard]] bool EmitObjectFile ( std::string_view Path );

        private:

            EmitterServices *Services = nullptr;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
