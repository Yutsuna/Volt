#pragma once

#include <cstdint>
#include <string_view>

namespace Volt
{

namespace CLI
{

    // Single source of truth for the toolchain version: the numeric parts.
    inline constexpr std::uint32_t VersionMajor = 0;
    inline constexpr std::uint32_t VersionMinor = 1;
    inline constexpr std::uint32_t VersionPatch = 0;

    // The combined numeric form, for easy comparisons.
    inline constexpr std::uint32_t CombinedVersion = ( VersionMajor << 16 ) | ( VersionMinor << 8 ) | VersionPatch;

    /// The display/manifest form, stamped into generated `runtime "..."` entries.
    inline constexpr std::string_view VoltVersion = "0.1.0";

} // namespace CLI

} // namespace Volt
