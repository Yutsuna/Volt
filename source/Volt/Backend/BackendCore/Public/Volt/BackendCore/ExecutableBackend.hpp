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
// per incremental unit.
//
// No toolchain type appears here, deliberately. The Driver includes this header
// to reach a JIT the way it includes TargetBackend.hpp to reach an AOT emitter,
// and it must keep being able to do that without an LLVM header entering its
// translation units (Driver.hpp: "Driver is the only place --target resolves to
// a concrete IBackend").

#include "BackendCore_export.hpp"
#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"

#include <cstddef>
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

        // Run the entry point. Once per build; a consumer that evaluates a
        // growing program one unit at a time uses EvalUnit instead.
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

        // Compile one more unit into an already-materialised build and run
        // its initialiser.
        //
        // `Build` is the compilation this unit came out of, and it is a
        // different object every time, for the same reason Reload takes one:
        // an incremental build grows its type store as it goes, and the view a
        // backend was begun with describes a build that has since gained units.
        //
        // Unlike Run, a failure here is a diagnostic and not the end of
        // anything: bOk == false with a Message means this unit did not run,
        // never that what is already resident has stopped working.
        [[nodiscard]] virtual RunResult EvalUnit ( const BackendInput &Build, const UnitView &Unit ) = 0;

        // The address a mangled symbol materialised at, for tests and
        // debugging. Zero when it does not resolve.
        [[nodiscard]] virtual std::uintptr_t LookupSymbol ( std::string_view Mangled ) = 0;

        // --- Inspection ----------------------------------------------------
        //
        // What a REPL's `:type`, `:ir`, `:asm` and `:bench` are built out of.
        // They are verbs on this seam rather than on a concrete JIT for the
        // same reason Run and EvalUnit are: no toolchain type appears in any
        // of their signatures, so a consumer can ask for the disassembly of a
        // function without an LLVM header entering its translation unit.

        // Emit one more unit exactly as EvalUnit would — same options, same
        // generator, same verification — and then throw the emission away.
        //
        // No dylib is opened, no module reaches ORC, nothing runs, and the
        // generator dies with the call. That is what makes it safe to ask a
        // question about a line the user never asked to evaluate: `:type` and
        // an abandoned completion both take this path, and neither can leave a
        // generation behind because neither ever opens one.
        //
        // With OutIr non-null, the emitted modules are rendered into it before
        // they are dropped.
        [[nodiscard]] virtual bool
        ProbeUnit ( const BackendInput &Build, const UnitView &Unit, std::string *OutIr, std::string &OutError ) = 0;

        // The intermediate representation of the last unit this backend added,
        // or empty when recording was never turned on. Costs a render per
        // emission, so it is off unless RecordIr says otherwise.
        [[nodiscard]] virtual std::string LastUnitIr () const = 0;

        virtual void RecordIr ( bool bEnable ) = 0;

        // Machine code at `Address`, as text, stopping at the first return or
        // after `MaxBytes`, whichever comes first. Empty when the target has no
        // disassembler built in.
        [[nodiscard]] virtual std::string Disassemble ( std::uintptr_t Address, std::size_t MaxBytes ) = 0;

        struct BenchResult
        {

            bool bOk = false;
            std::string Message;
            std::size_t Iterations   = 0;
            std::uint64_t TotalNanos = 0;
            std::uint64_t BestNanos  = 0;
        };

        // Emit one unit, run its initialiser `Iterations` times under a
        // monotonic clock, and drop the generation it ran in.
        //
        // The generation is opened as a *replacement* — a dylib of its own —
        // and removed the moment the last iteration returns, so a hundred
        // benchmarks leave the session exactly as wide as they found it. The
        // timing loop is here rather than in a caller because this is the only
        // place that can drop what it opened.
        [[nodiscard]] virtual BenchResult
        BenchUnit ( const BackendInput &Build, const UnitView &Unit, std::size_t Iterations ) = 0;

        // How many generations are still resident. The observable half of the
        // rule above: it does not move across a `:type`, and it comes back to
        // where it started across a `:bench`.
        [[nodiscard]] virtual std::size_t LiveGenerations () const = 0;
    };

} // namespace Backend

} // namespace Volt
