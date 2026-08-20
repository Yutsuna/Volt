#pragma once

// PhaseTimer.hpp — the wall-clock the compiler did not have.
//
// `PassStats` counts *what* a pass did; nothing measured *how long*. That gap
// is why "the compiler must stay fast" was, until now, an intention rather
// than a check: a change could double a seam's cost and no test would notice.
//
// The shape is deliberately the cheapest thing that answers the question: a
// process-wide table of named accumulators, one RAII scope per seam. Timing a
// seam costs two `steady_clock::now()` calls and one string-keyed add — a
// seam runs O(1) times per build (it brackets a whole phase, never a node),
// so the instrument can never itself become the cost it measures.
//
// Recording is opt-in (`SetEnabled`), so a build that does not ask pays only
// the branch. Accumulation is mutex-guarded because the Driver brackets seams
// from inside its jthread pool.

#include "Core_export.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Core
{

    // One named accumulator: total wall time and how many scopes fed it.
    struct PhaseSample
    {

        std::string Name;
        double Milliseconds = 0.0;
        std::size_t Count   = 0;
    };

    // The process-wide table. Insertion order is preserved, so a report reads
    // in pipeline order rather than alphabetically — which is the order a
    // reader needs to spot *where* a build went slow.
    //
    // Every member is defined in PhaseTimer.cpp, not here, and the class
    // carries CORE_EXPORT. That is load-bearing under `default_library=shared`
    // (rules/shared-lib-exports.md): the table has to be *one* object for the
    // whole process, and a header-only `static` local inside an inline member
    // is emitted once per module with `gnu_symbol_visibility: hidden` — so
    // `volt` would flip its own copy's flag while `libDriver.so` kept
    // recording into a different, still-disabled one. Which is exactly the
    // silence this fixed.
    class CORE_EXPORT PhaseTimings
    {

    public:

        static void SetEnabled ( bool bValue );

        [[nodiscard]] static bool Enabled ();

        static void Record ( std::string_view Name, double Milliseconds );

        // A copy, not a reference: the caller reports outside the lock.
        [[nodiscard]] static std::vector<PhaseSample> Snapshot ();

        static void Reset ();
    };

    // Brackets one pipeline seam. Non-copyable, non-movable: its whole
    // contract is that it dies exactly where it was declared.
    class PhaseScope
    {

    public:

        explicit PhaseScope ( std::string_view InName ) : Name( InName ), Begin( std::chrono::steady_clock::now() )
        {
        }

        PhaseScope ( const PhaseScope & )           = delete;
        PhaseScope &operator=( const PhaseScope & ) = delete;
        PhaseScope ( PhaseScope && )                = delete;
        PhaseScope &operator=( PhaseScope && )      = delete;

        // Record early and disarm. Idempotent, and the destructor honours it —
        // for a seam whose natural `{}` cannot be drawn, because declarations
        // made inside it (the Driver's `UnitAsts`) outlive it.
        void Stop ()
        {
            if ( bStopped )
            {
                return;
            }
            bStopped                                                = true;
            const std::chrono::duration<double, std::milli> Elapsed = std::chrono::steady_clock::now() - Begin;
            PhaseTimings::Record( Name, Elapsed.count() );
        }

        ~PhaseScope ()
        {
            Stop();
        }

    private:

        std::string_view Name;
        std::chrono::steady_clock::time_point Begin;
        bool bStopped = false;
    };

    // One line per recorded seam, in pipeline order, plus a total. Returned
    // rather than printed: Core does not own the CLI's logger conventions,
    // and `check`/`build` each stamp their own step name.
    [[nodiscard]] CORE_EXPORT std::vector<std::string> FormatPhaseTimings ();

} // namespace Core

} // namespace Volt
