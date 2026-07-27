#pragma once

#include "Volt/CLI/GenericCommand.hpp"
#include "Volt/CLI/StdlibCache.hpp"

#include <string>

namespace Volt
{

namespace CLI
{

    /**
     * @class FCheckCommand
     * @usage
     *        volt check [options] [input_file_or_dir]
     * @description
     *        Static semantic code analysis. When the input belongs to a circuit
     *        (a Project.vl manifest is found upward), the whole circuit is analysed.
     * @options
     *        -i INPUT, --input INPUT          Code target directory or source file
     *        --type TYPE                      Type verification scope (syntax|semantic|style)
     *        --rules RULES                    Location path defining validation rule models
     *        --output OUT                     Output validation layout formats
     *        --warn-as-error                  Style warning events will promote to runtime errors
     *        --metrics                        Gather code statistics and size metric benchmarks
     *        --unused                         Flag unreferenced syntax structures and bindings
     *        -h, --help                       Show help
     */
    class FCheckCommand : public IGenericCommand
    {

    public:

        [[nodiscard]] std::int32_t Execute ( std::span<const std::string_view> InArgs ) override;

    public:

        [[nodiscard]] std::string_view GetName () const noexcept override;
        [[nodiscard]] std::string_view GetDescription () const noexcept override;
        [[nodiscard]] std::string_view GetUsage () const noexcept override;
        [[nodiscard]] std::vector<FOption> GetOptions () override;

    private:

        std::string Input;
        std::string Type = "syntax";
        std::string Rules;
        std::string OutputFormat = "json";

        bool bWarnAsError = false;
        bool bMetrics     = false;
        bool bUnused      = false;
        bool bVerbose     = false;

        FStdlibCacheFlags StdlibFlags;
    };

} // namespace CLI

} // namespace Volt
