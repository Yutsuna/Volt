#pragma once

// LlvmEmitter.hpp — the TargetBackend behind `volt build` (native AOT).
//
// No LLVM header leaks out of this module: the public surface is plain
// Volt types and the LLVM state hides behind a pimpl, so nothing upstream
// recompiles against the LLVM API and the module stays optional
// (VOLT_ENABLE_LLVM). Emission is specified in .agents/backend/llvm.md.

#include "BackendLLVM_export.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        // How far Finalize takes the module. `--emit ir`/`--emit obj` stop
        // early; the default (Link) runs the whole way to a linked artifact.
        // Plain data — no LLVM type appears here, so this stays in the public
        // header for `volt build` (BuildCommand) to fill in from its CLI flags.
        enum class EEmitStage : std::uint8_t
        {

            Ir     = 0,
            Object = 1,
            Link   = 2,
        };

        struct EmitOptions
        {

            EEmitStage Stage      = EEmitStage::Link;
            std::uint8_t OptLevel = 0;
            bool bLto             = false;
            bool bDebugInfo       = true;
            // Where Finalize's artifact lands. Empty means "derive from the
            // module name", since a caller may not know it in advance.
            std::string OutputPath;
            // The Volt free function the C runtime starts the program at, and
            // the C symbol it must be reachable under. Every Volt function is
            // mangled (BackendCore/Mangler.hpp), so without a shim there is no
            // `main` for the CRT to call at all. Which name means "entry" is a
            // *language* convention, not a code-generation one, so it is set
            // here by `volt build` rather than assumed by the emitter; empty
            // means "emit no entry point", which is what a library build is.
            std::string EntryFunction = "main";
            std::string EntrySymbol   = "main";
        };

        class BACKENDLLVM_EXPORT LlvmBackend
        {

        public:

            LlvmBackend ();
            ~LlvmBackend ();
            LlvmBackend ( LlvmBackend && ) noexcept;
            LlvmBackend &operator=( LlvmBackend && ) noexcept;

            [[nodiscard]] std::string_view Name () const
            {
                return "llvm";
            }

            // Must be called before Begin(); Finalize() reads it to decide how
            // far to take the module and where the artifact goes.
            void SetOptions ( EmitOptions InOptions );

            void Begin ( const BackendInput &Input );

            [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit );

            [[nodiscard]] EmitResult Finalize ();

        private:

            struct State;
            std::unique_ptr<State> Impl;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
