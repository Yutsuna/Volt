#pragma once

#include "Volt/CLI/CommandInputs.hpp"
#include "Volt/CLI/GenericCommand.hpp"
#include "Volt/CLI/StdlibCache.hpp"

#include <string>
#include <vector>

namespace Volt
{

namespace CLI
{

    /**
     * @class FRunCommand
     * @usage
     *        volt run [options] [input_file] [-- program_args...]
     * @description
     *        Compile the input and run it in this process through
     *        Driver::Run(), which resolves --target to BackendJIT — no
     *        artifact is written and no linker is involved. Like
     *        FBuildCommand, this command never includes a Backend* header.
     *        The command's own exit status is the program's exit code, so
     *        `volt run` composes in a shell the way running the built
     *        artifact would.
     * @options
     *        -i INPUT, --input INPUT          File input source program
     *        --target TARGET                  Code generation target (native)
     *        -O LEVEL                         Optimization level (0|1|2|3, default 0)
     *        --load PATH                      Resolve symbols against a shared object first
     *        --stdin                          Read source from standard input
     *        -v, --verbose                    Enable verbose output
     *        --no-stdlib-cache                Bypass the frontend stdlib cache
     *        --fresh-stdlib                   Force-refresh the stdlib cache
     *        --no-stdlib                      Skip the stdlib entirely
     *        -h, --help                       Show help
     */
    class FRunCommand : public IGenericCommand
    {

    public:

        [[nodiscard]] std::int32_t Execute ( std::span<const std::string_view> InArgs ) override;

    public:

        [[nodiscard]] std::string_view GetName () const noexcept override;
        [[nodiscard]] std::string_view GetDescription () const noexcept override;
        [[nodiscard]] std::string_view GetUsage () const noexcept override;
        [[nodiscard]] std::vector<FOption> GetOptions () override;

    private:

        FInputFlags InputFlags;

        std::string Target = "native";
        std::string OptLevel;
        std::vector<std::string> Dylibs;

        bool bVerbose       = false;
        bool bWholeModule   = false;
        bool bIndirectCalls = false;
        bool bWatch         = false;

        FStdlibCacheFlags StdlibFlags;
    };

} // namespace CLI

} // namespace Volt
