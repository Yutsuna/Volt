#pragma once

#include "Volt/CLI/GenericCommand.hpp"

#include <string>

namespace Volt
{

namespace CLI
{

    /**
     * @class FCircuitCommand
     * @usage
     *        volt circuit [options]
     * @description
     *        Create or update the Project.vl file: scans `Source/` for module
     *        directories, preserves any existing manifest entries, and flags
     *        (without deleting) modules or entrypoints that disappeared.
     * @options
     *        -d DIR, --dir DIR                Project directory path
     *        -h, --help                       Show help
     */
    class FCircuitCommand : public IGenericCommand
    {

    public:

        [[nodiscard]] std::int32_t Execute ( std::span<const std::string_view> InArgs ) override;

    public:

        [[nodiscard]] std::string_view GetName () const noexcept override;
        [[nodiscard]] std::string_view GetDescription () const noexcept override;
        [[nodiscard]] std::string_view GetUsage () const noexcept override;
        [[nodiscard]] std::vector<FOption> GetOptions () override;

    private:

        std::string ProjectDirectory;
    };

} // namespace CLI

} // namespace Volt
