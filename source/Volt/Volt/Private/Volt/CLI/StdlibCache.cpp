#include "Volt/CLI/StdlibCache.hpp"

std::vector<Volt::CLI::FOption> Volt::CLI::StdlibCacheOptions ( FStdlibCacheFlags &Flags )
{
    // clang-format off
    return {
        {
            "", "--no-stdlib-cache", "", "Bypass frontend + native stdlib caches; always recompile fresh",
            [&Flags] ( std::string_view ) { Flags.bNoStdlibCache = true; }
        },
        {
            "", "--fresh-stdlib", "", "Force-discard and recompute the stdlib cache for the current key",
            [&Flags] ( std::string_view ) { Flags.bFreshStdlib = true; }
        },
        {
            "", "--no-stdlib", "", "Skip the stdlib entirely (for building source/Lib/** itself)",
            [&Flags] ( std::string_view ) { Flags.bNoStdlib = true; }
        },
        {
            "", "--stdlib-artifact", "KIND", "Native stdlib artifact kind to build/consume (static|shared)",
            [&Flags] ( std::string_view Val ) { Flags.Artifact = Val; }
        }
    };
    // clang-format on
}

Volt::Driver::FCacheOptions Volt::CLI::ToDriverCacheOptions ( const FStdlibCacheFlags &Flags )
{
    return { .bNoCache  = Flags.bNoStdlibCache,
             .bFresh    = Flags.bFreshStdlib,
             .bNoStdlib = Flags.bNoStdlib,
             .bVerbose  = Flags.bVerbose };
}
