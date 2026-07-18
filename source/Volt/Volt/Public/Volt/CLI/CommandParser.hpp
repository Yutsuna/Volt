#pragma once

#include "Volt/CLI/GenericCommand.hpp"

#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace Volt
{

namespace CLI
{

    namespace CommandParser
    {

        using FParseResult = std::expected<std::vector<std::string_view>, std::string>;

        /**
         * @brief Parses command-line arguments and options.
         * @param InArgs A span of string views representing the command-line arguments.
         * @param InOptions A span of FOption representing the available options for the command.
         * @return An expected object containing a vector of string views representing the parsed arguments on success,
         *         or a string error message on failure.
         */
        FParseResult Parse ( std::span<const std::string_view> InArgs, std::span<const FOption> InOptions );

    } // namespace CommandParser

} // namespace CLI

} // namespace Volt
