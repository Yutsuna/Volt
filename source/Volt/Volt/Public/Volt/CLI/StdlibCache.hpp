#pragma once

// StdlibCache.hpp — the reusable stdlib cache *flags* every compiling
// command shares (issue #61, Phase 5). Declared once here so `run`/`repl`
// pick them up with no rework the moment those commands exist, per
// cli-surface.md's meta-first shape: one option-group function, reused by
// reference, never copy-pasted per command.
//
// Deliberately backend- and Driver-internals-agnostic: this file knows
// Driver::FCacheOptions (the frontend/sema cache) and nothing else. Whether
// a *native* artifact cache exists at all, and what it looks like (an LLVM
// object archive today; bytecode for a future VM-only build, a .wasm blob
// for BackendWASM), is Driver::Build()'s own concern (Driver/Private/
// DriverBuild.cpp) — Volt itself never includes a Backend* header at all.
// `Artifact` here is therefore opaque: this file reads it from nowhere and
// interprets it nowhere, only carries it from the CLI flag to
// Driver::BuildOptions::StdlibArtifactKind.

#include "Volt/CLI/GenericCommand.hpp"
#include "Volt/Driver/Driver.hpp"

#include <string>
#include <vector>

namespace Volt
{

namespace CLI
{

    // Backing storage for the shared flags below. A command embeds one as a
    // member and passes it to StdlibCacheOptions(); GetOptions() then
    // appends that vector to its own. `Artifact` is read (never interpreted)
    // by this file — a concrete backend's own build mode decides what values
    // it accepts.
    struct FStdlibCacheFlags
    {

        bool bNoStdlibCache  = false;    // --no-stdlib-cache
        bool bFreshStdlib    = false;    // --fresh-stdlib
        bool bNoStdlib       = false;    // --no-stdlib
        std::string Artifact = "static"; // --stdlib-artifact=<backend-defined>
    };

    // The shared row set: --no-stdlib-cache, --fresh-stdlib, --no-stdlib,
    // --stdlib-artifact. `Flags` must outlive every FOption's callback, so a
    // caller passes in a member of the owning command, never a local.
    [[nodiscard]] std::vector<FOption> StdlibCacheOptions ( FStdlibCacheFlags &Flags );

    // The frontend/sema half of Flags, translated into what
    // Driver::CompileFiles/CompileCircuit consume directly.
    [[nodiscard]] Driver::FCacheOptions ToDriverCacheOptions ( const FStdlibCacheFlags &Flags );

} // namespace CLI

} // namespace Volt
