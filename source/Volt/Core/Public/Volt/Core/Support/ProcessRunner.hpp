#pragma once

#include "Core_export.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Volt
{

namespace Core
{

    // What a compile-time command is allowed to cost. Both limits are hard: a
    // build must fail loudly on a command that hangs or floods, never wait on
    // one. The defaults are generous for `git rev-parse` / `find` and still
    // bounded enough that a runaway command cannot wedge a build machine.
    struct ProcessLimits
    {

        std::uint32_t TimeoutMs = 10'000;
        std::size_t MaxBytes    = 1U << 20; // per stream
    };

    // The whole outcome, never a partial one: a caller that only checks Out
    // would otherwise treat a timeout as an empty command. `bSpawnFailed`
    // separates "the host could not start a shell" from "the command ran and
    // failed", which are different diagnostics.
    struct ProcessResult
    {

        int ExitCode      = -1;
        std::string Out;  // captured stdout, truncated at MaxBytes
        std::string Err;  // captured stderr, truncated at MaxBytes
        bool bTimedOut    = false;
        bool bTruncated   = false;
        bool bSpawnFailed = false;

        [[nodiscard]] bool Ok () const
        {
            return not bSpawnFailed and not bTimedOut and ExitCode == 0;
        }
    };

    /// Run Command through the host shell, from WorkDir, capturing both
    /// streams. Never throws, never blocks past Limits.TimeoutMs, never reads
    /// past Limits.MaxBytes per stream. A failure is reported in the result,
    /// not by an exception: the caller is a compiler pass and turns it into a
    /// diagnostic.
    [[nodiscard]] CORE_EXPORT ProcessResult
    RunShell ( std::string_view Command, std::string_view WorkDir, ProcessLimits Limits = {} );

} // namespace Core

} // namespace Volt
