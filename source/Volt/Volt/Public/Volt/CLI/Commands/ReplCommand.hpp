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
     *        header — Repl::Evaluator is the one place a target resolves to a
     *        concrete backend.
     *
     *        Two front ends, chosen by what standard input is. On a terminal
     *        the session is handed to Repl::Tui: raw mode, live syntax
     *        colouring, semantic completion, history and a side panel for
     *        `:src` / `:doc` / `:ir` / `:asm`. Piped — or under `-e` — it
     *        reads the script and evaluates it line by line with no prompt
     *        and not one escape sequence anywhere, which is the shape the
     *        test suite drives.
     *
     *        The `:` builtins answer on both paths. They are questions about
     *        the session rather than a feature of the terminal, so a script
     *        can ask them, and the tests that pin their answers run where
     *        there is no colour to strip out of a golden file.
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
