#pragma once

#include "Volt/CLI/GenericCommand.hpp"

#include <string>

namespace Volt
{

namespace CLI
{

    /**
     * @class FParseCommand
     * @usage
     *        volt parse <input> [options]
     * @description
     *        Parses the specified volt input and outputs the result in the specified format.
     * @options
     *        -i INPUT, --input INPUT          Source input module path
     *        -o OUTPUT, --output OUTPUT       Output target path structure
     *        --format FORMAT                  Serialization formats (json|dot|text)
     *        --simplify                       Deduplicate structural tree layout elements
     *        --no-color                       Output without colors
     *        --no-location                    Omit character and index coordinates
     *        -h, --help                       Show help
     */
    class FParseCommand : public IGenericCommand
    {

    public:

        [[nodiscard]] int32_t Execute ( std::span<const std::string_view> InArgs ) override;

    public:

        [[nodiscard]] std::string_view GetName () const noexcept override;
        [[nodiscard]] std::string_view GetDescription () const noexcept override;
        [[nodiscard]] std::vector<FOption> GetOptions () override;

    private:

        std::string Input;
        std::string Output;
        std::string Format = "json";

        bool bSimplify   = false;
        bool bNoColor    = false;
        bool bNoLocation = false;
    };

} // namespace CLI

} // namespace Volt
