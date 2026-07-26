#pragma once

#include "Volt/CLI/GenericCommand.hpp"

#include <string>

namespace Volt
{

namespace CLI
{

    /**
     * @class FBuildCommand
     * @usage
     *        volt build [options] [input_file]
     * @description
     *        Compile the file input to a native artifact via BackendLLVM.
     *        When the input belongs to a circuit (a Project.vl manifest is
     *        found upward), the whole circuit is built.
     * @options
     *        -i INPUT, --input INPUT          File input source program
     *        -o OUTPUT, --output OUTPUT       Output artifact path
     *        --target TARGET                  Code generation target (native|wasm)
     *        -O LEVEL                         Optimization level (0|2|3)
     *        --emit KIND                      Stop after an intermediate artifact (ir|obj)
     *        --lto                            Enable link-time optimization (native only)
     *        -h, --help                       Show help
     */
    class FBuildCommand : public IGenericCommand
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
        std::string Output;
        std::string Target = "native";
        std::string OptLevel;
        std::string Emit;

        bool bLto = false;
    };

} // namespace CLI

} // namespace Volt
