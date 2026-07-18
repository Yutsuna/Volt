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
     * @brief One CLI option: names, optional value placeholder, description, callback.
     * @details A non-empty ValueName ("INPUT") makes the option consume the next
     *          argument, which is handed to the callback; flags get "".
     */
    struct FOption
    {

        std::string_view ShortName; //<< "-i", or empty when the option has no short form
        std::string_view LongName;  //<< "--input"
        std::string_view ValueName; //<< "INPUT", or empty for a plain flag
        std::string_view Description;
        std::function<void( std::string_view )> Callback;

        [[nodiscard]] bool HasValue () const noexcept
        {
            return !ValueName.empty();
        }
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
        [[nodiscard]] virtual std::int32_t Execute ( std::span<const std::string_view> InArgs ) = 0;

    public:

        [[nodiscard]] virtual std::string_view GetName () const noexcept        = 0;
        [[nodiscard]] virtual std::string_view GetDescription () const noexcept = 0;
        [[nodiscard]] virtual std::string_view GetUsage () const noexcept       = 0;
        [[nodiscard]] virtual std::vector<FOption> GetOptions ()                = 0;

    protected:

        static constexpr std::int32_t ExitSuccess = 0;
        static constexpr std::int32_t ExitFailure = 84;
    };

} // namespace CLI

} // namespace Volt
