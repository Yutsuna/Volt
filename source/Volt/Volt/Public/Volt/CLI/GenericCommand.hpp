#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace Volt
{

namespace CLI
{

    /**
     * @struct FOption
     * @brief Represents CLI option with its name, description, and callback function.
     */
    struct FOption
    {
        std::string_view ShortName;
        std::string_view LongName;
        std::string_view Description;
        bool bHasValue = false;
        std::function<void( std::string_view )> Callback;
    };

    /**
     * @class IGenericCommand
     * @brief Interface for a generic command in the CLI.
     */
    class IGenericCommand
    {
    public:

        virtual ~IGenericCommand () noexcept = default;

    public:

        /**
         * @brief Executes the command with the provided arguments.
         * @param InArgs A span of string views representing the command-line arguments.
         * @return An integer status code indicating the result of the command execution.
         */
        [[nodiscard]] virtual int32_t Execute ( std::span<const std::string_view> InArgs ) = 0;

    public:

        [[nodiscard]] virtual std::string_view GetName () const noexcept        = 0;
        [[nodiscard]] virtual std::string_view GetDescription () const noexcept = 0;
        [[nodiscard]] virtual std::vector<FOption> GetOptions ()                = 0;

    protected:

        static constexpr int32_t ExitSuccess = 0;
        static constexpr int32_t ExitFailure = 1;
    };

} // namespace CLI

} // namespace Volt
