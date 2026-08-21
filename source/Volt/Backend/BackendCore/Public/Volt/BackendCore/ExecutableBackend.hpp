#pragma once

// ExecutableBackend.hpp — the seam for a backend that runs instead of writing.
//
// `TargetBackend` (and the `IBackend` hop the Driver picks a target through)
// describes a generator that ends in an *artifact path*. A JIT ends in a
// running program, so it needs three more verbs and one accessor; everything
// else — Begin / EmitUnit / Finalize — it satisfies unchanged.
//
// Those verbs are virtual, which is within rules the rest of the backend keeps:
// the ban is on a virtual call *per node*, never per execution
// (backend/core-interfaces.md). Run happens once per `volt run`; EvalUnit once
// per REPL line.
//
// No toolchain type appears here, deliberately. The Driver includes this header
// to reach a JIT the way it includes TargetBackend.hpp to reach an AOT emitter,
// and it must keep being able to do that without an LLVM header entering its
// translation units (Driver.hpp: "Driver is the only place --target resolves to
// a concrete IBackend").

#include "BackendCore_export.hpp"
#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Volt
{

namespace Backend
{

    // What one execution of the compiled program produced. `Code` is the Volt
    // program's own exit status, meaningful only when bOk; `Message` is the
    // host-side failure (a symbol that would not resolve, a session that was
    // never finalised), never a Volt source diagnostic.
    struct RunResult
    {

        bool bOk          = false;
        std::int32_t Code = 0;
        std::string Message;
    };

    enum class EReloadStatus : std::uint8_t
    {

        Ok = 0,
        // The new code is incompatible with what is already running — a
        // signature or a layout moved. `Message` names what, and the caller's
        // fallback is a full restart, which this target makes cheap.
        Refused = 1,
        Error   = 2,
    };

    struct ReloadResult
    {

        EReloadStatus Status = EReloadStatus::Error;
        std::string Message;
        std::size_t PatchedSymbols = 0;
    };

    // A backend that executes what it emitted. Extends the runtime seam rather
    // than replacing it: Begin / EmitUnit / Finalize prepare exactly as they do
    // for an AOT target, and the verbs below are what happens afterwards.
    class BACKENDCORE_EXPORT IJitBackend : public IBackend
    {

    public:

        ~IJitBackend () override = default;

        // Run the entry point. Once per session in `run` mode; a REPL uses
        // EvalUnit instead.
        [[nodiscard]] virtual RunResult Run ( std::span<const std::string_view> ProgramArgs ) = 0;

        // Recompile an already-emitted unit and repoint its symbols. Requires a
        // session built with per-unit modules and indirect linkage; refuses,
        // with a message saying so, otherwise.
        //
        // `Build` is the *new* compilation the unit came out of, not the one
        // this backend was begun with: recompiling one file means recompiling
        // the front end, and what comes back is a whole new type store with
        // ids of its own. The running program keeps the old one — its
        // instances are laid out to it — which is exactly what makes the two
        // comparable, and what a refusal compares.
        [[nodiscard]] virtual ReloadResult Reload ( const BackendInput &Build, const UnitView &Unit ) = 0;

        // Compile one incremental unit and run its initialiser — one REPL line.
        [[nodiscard]] virtual RunResult EvalUnit ( const UnitView &Unit ) = 0;

        // The address a mangled symbol materialised at, for tests and
        // debugging. Zero when it does not resolve.
        [[nodiscard]] virtual std::uintptr_t LookupSymbol ( std::string_view Mangled ) = 0;
    };

} // namespace Backend

} // namespace Volt
