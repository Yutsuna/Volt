#pragma once

#include "Volt/CLI/GenericCommand.hpp"

namespace Volt
{

namespace CLI
{

    /**
     * @class FHelpCommand
     * @usage
     *        volt help
     * @description
     *        Display the usage: every registered command with its description.
     */
    class FHelpCommand : public IGenericCommand
    {

    public:

        [[nodiscard]] std::int32_t Execute ( std::span<const std::string_view> InArgs ) override;

    public:

        [[nodiscard]] std::string_view GetName () const noexcept override;
        [[nodiscard]] std::string_view GetDescription () const noexcept override;
        [[nodiscard]] std::string_view GetUsage () const noexcept override;
        [[nodiscard]] std::vector<FOption> GetOptions () override;
    };

} // namespace CLI

} // namespace Volt
