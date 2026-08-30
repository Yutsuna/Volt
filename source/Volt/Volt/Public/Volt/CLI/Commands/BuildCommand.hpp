#pragma once

#include "Volt/CLI/CommandInputs.hpp"
#include "Volt/CLI/GenericCommand.hpp"
#include "Volt/CLI/StdlibCache.hpp"

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
     *        Compile the file input to a native artifact through
     *        Driver::Build() — the one place --target resolves to a
     *        concrete backend (BackendLLVM for "native" today); this
     *        command itself never includes a Backend* header.
     *        When the input belongs to a circuit (a Project.vl manifest is
     *        found upward), the whole circuit is built.
     *        When --stdin is used, source is read from standard input and
     *        compiled as a single file (no circuit manifest lookup).
     * @options
     *        -i INPUT, --input INPUT          File input source program
     *        -o OUTPUT, --output OUTPUT       Output artifact path
     *        --target TARGET                  Code generation target (native|wasm)
     *        -O LEVEL                         Optimization level (0|1|2|3, default 2)
     *        --emit KIND                      Stop after an intermediate artifact (ast|ir|asm|obj)
     *        --syntax SYNTAX                  Assembly syntax dialect (att|intel|arm)
     *        --lto                            Enable link-time optimization (native only)
     *        --stdin                          Read source from standard input
     *        -v, --verbose                    Enable verbose output
     *        --no-stdlib-cache                Bypass frontend + native stdlib caches
     *        --fresh-stdlib                   Force-refresh the stdlib cache
     *        --no-stdlib                      Skip the stdlib entirely
     *        --stdlib-artifact KIND            Native stdlib artifact kind (static|shared)
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

        FInputFlags InputFlags;

        std::string Output;
        std::string Target = "native";
        std::string OptLevel;
        std::string Emit;
        std::string AsmSyntax;

        bool bLto     = false;
        bool bVerbose = false;

        FStdlibCacheFlags StdlibFlags;
    };

} // namespace CLI

} // namespace Volt
