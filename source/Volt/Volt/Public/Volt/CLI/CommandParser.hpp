#pragma once

#include "Volt/CLI/GenericCommand.hpp"

#include <expected>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace CLI
{

    namespace CommandParser
    {

        /**
         * @struct FParsedArgs
         * @brief Result of a successful parse: leftover positionals + help request.
         */
        struct FParsedArgs
        {

            std::vector<std::string_view> Positionals;
            bool bHelpRequested = false;
        };

        using FParseResult = std::expected<FParsedArgs, std::string>;

        /**
         * @brief Parses command-line arguments and options.
         * @param InArgs A span of string views representing the command-line arguments.
         * @param InOptions A span of FOption representing the available options for the command.
         * @return The positional arguments (and whether `-h`/`--help` was seen, which is
         *         recognised for every command) on success, or an error message on failure.
         */
        FParseResult Parse ( std::span<const std::string_view> InArgs, std::span<const FOption> InOptions );

        /**
         * @brief Renders an OptionParser-style usage block: banner, then aligned options.
         * @param Out The output stream to print to.
         * @param InUsage The banner line, e.g. "volt parse [options] [input_file]".
         * @param InOptions The command's options; `-h, --help` is appended automatically.
         */
        void PrintUsage ( std::ostream &Out, std::string_view InUsage, std::span<const FOption> InOptions );

    } // namespace CommandParser

} // namespace CLI

} // namespace Volt
