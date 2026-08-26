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
#include <vector>

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
            std::string EntryFunction = "__volt_entry";
            std::string EntrySymbol   = "main";

            // --- Issue #61: stdlib native-artifact cache -----------------
            //
            // When true, a stdlib-owned member (its declaring unit's ordinal
            // is below BackendInput::StdlibUnitCount) is left declared —
            // DeclareAll already declares every concrete member store-wide —
            // but never defined: EmitUnit skips DefineAll for a stdlib unit
            // exactly the way an @[External] member is never defined,
            // because its body is expected to come from a linked precompiled
            // archive/shared object instead. Set by the consuming build
            // (ordinary `volt build`), never by the build that produces the
            // artifact itself (that one defines the stdlib as usual, just
            // with no entry point and Stage == Object).
            bool bStdlibPrecompiled = false;

            // Extra linker inputs — e.g. the precompiled stdlib
            // `native/<NativeCacheKey>.a`/`.so` — inserted right after the
            // primary object file and before every `-l`, so a static archive
            // among them can resolve the symbols this object references.
            std::vector<std::string> ExtraLinkInputs;

            // Stage == Link only: build `-shared -fPIC` (no CRT entry, no
            // `main` shim — EntrySymbol is expected empty alongside this)
            // instead of a linked executable. What the archive-cache build
            // mode sets to produce `native/<NativeCacheKey>.so` (Phase 4).
            bool bSharedOutput = false;

            // Emit a definition of the unwind-slot accessor beside this
            // module's transport globals. Set when the artifact being produced
            // is one a JIT will later load and skip over: without it, JIT-ed
            // code and this artifact would each hold their own copy of the
            // in-flight exception state (Volt/BackendCore/UnwindTransport.hpp).
            bool bDefineSlotAccessor = false;

            // Keep and export mergeable definitions instead of letting the
            // linker discard the ones that got fully inlined. Set for an
            // artifact a JIT will load and skip over.
            bool bRetainMergeableBodies = false;
        };

        // The host triple Begin() would select — plain data, no LLVM type
        // involved, so a caller (issue #61's native-artifact cache key) can
        // fold it into a cache key without ever touching llvm::sys itself.
        [[nodiscard]] BACKENDLLVM_EXPORT std::string DefaultTargetTriple ();

        // Compiles Build's whole unit set as one precompiled stdlib artifact
        // published at TargetPath: a static archive (bShared == false, via
        // `ar rcs`) or a `-shared -fPIC` object (bShared == true), either way
        // an ordinary EmitOptions{ .EntrySymbol = "" } library build
        // published atomically (temp file, then rename — a concurrent reader
        // only ever observes a complete artifact). When MetaPath is
        // non-empty, also writes a best-effort `.meta` symbol manifest
        // (dumped via `nm`; never a hard failure if `nm` is unavailable) —
        // issue #61 Phase 3's native-artifact cache build mode.
        //
        // Every raw LLVM call this needs (ar/nm process execution, temp
        // files, the target triple) stays inside this module: a caller,
        // however LLVM-unaware, only ever sees BackendInput/EmitOptions/a
        // bool back, the same boundary LlvmBackend itself already keeps
        // (module comment above).
        [[nodiscard]] BACKENDLLVM_EXPORT bool BuildStdlibArtifact ( const BackendInput &Build,
                                                                    std::uint8_t OptLevel,
                                                                    bool bLto,
                                                                    bool bShared,
                                                                    std::string_view TargetPath,
                                                                    std::string_view MetaPath = {} );

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
