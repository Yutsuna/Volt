#pragma once

#include <cstdint>
#include <string_view>

namespace Volt
{

namespace Driver
{

    // The names the Driver recognises when it reads a project: file
    // extensions, circuit-manifest keys, and driver-understood annotations.
    // This is the *only* place such spellings live — the manifest walk and
    // the link-graph builder consume these constants, so extending the
    // circuit vocabulary is one line here + one clause at the use site.
    namespace WellKnown
    {

        // Source-file extensions.
        inline constexpr std::string_view SourceExt    = ".vl";
        inline constexpr std::string_view ComponentExt = ".vlx";

        // The circuit manifest file, as found at a project root.
        inline constexpr std::string_view ManifestName = "Project.vl";

        // `circuit "Name" { ... }` manifest keys.
        inline constexpr std::string_view RuntimeKey    = "runtime";
        inline constexpr std::string_view EntrypointKey = "entrypoint";
        inline constexpr std::string_view ModulesKey    = "modules";

        // The conventional project layout `volt circuit` scans and generates.
        inline constexpr std::string_view SourceDirName = "Source";
        inline constexpr std::string_view DefaultEntry  = "Source/Main.vl";

        // Top-level annotations the Driver itself interprets.
        inline constexpr std::string_view LinkAnnotation = "Link";

        // Fallback circuit name when the manifest declares none.
        inline constexpr std::string_view DefaultCircuitName = "@circuit";

    } // namespace WellKnown

} // namespace Driver

} // namespace Volt
