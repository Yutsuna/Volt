#pragma once

#include "Volt/CLI/GenericCommand.hpp"
#include "Volt/CLI/StdlibCache.hpp"

#include <string>
#include <vector>

namespace Volt
{

namespace CLI
{

    /**
     * @class FReplCommand
     * @usage
     *        volt repl [options]
     * @description
     *        Start an interactive session: read a line, compile it into a
     *        Driver that stays alive, and evaluate it through BackendJIT.
     *        Like every other command here, it never includes a Backend*
     *        header — Driver::ReplSession is the one place a target resolves
     *        to a concrete backend.
     *
     *        With standard input on a terminal the session is interactive.
     *        Piped, it reads the whole script and evaluates it line by line
     *        with no prompt and no colour, which is the shape the test suite
     *        drives.
     * @options
     *        -O LEVEL                         Optimization level (0|1|2|3, default 0)
     *        -e EXPR, --eval EXPR             Evaluate one line and exit
     *        -v, --verbose                    Enable verbose output
     *        --no-stdlib-cache                Bypass the frontend stdlib cache
     *        --fresh-stdlib                   Force-refresh the stdlib cache
     *        --no-stdlib                      Skip the stdlib entirely
     *        -h, --help                       Show help
     */
    class FReplCommand : public IGenericCommand
    {

    public:

        [[nodiscard]] std::int32_t Execute ( std::span<const std::string_view> InArgs ) override;

    public:

        [[nodiscard]] std::string_view GetName () const noexcept override;
        [[nodiscard]] std::string_view GetDescription () const noexcept override;
        [[nodiscard]] std::string_view GetUsage () const noexcept override;
        [[nodiscard]] std::vector<FOption> GetOptions () override;

    private:

        std::string OptLevel;
        std::vector<std::string> EvalLines;

        bool bVerbose = false;

        FStdlibCacheFlags StdlibFlags;
    };

} // namespace CLI

} // namespace Volt
