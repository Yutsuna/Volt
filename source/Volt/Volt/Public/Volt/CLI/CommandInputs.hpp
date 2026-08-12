#pragma once

#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/GenericCommand.hpp"

#include <filesystem>

namespace Volt
{

namespace CLI
{

    /**
     * @struct FInputFlags
     * @brief Storage for input options (-i / --input and --stdin).
     */
    struct FInputFlags
    {
        std::string ExplicitInput;
        bool bStdin = false;
    };

    /**
     * @struct FTempInputGuard
     * @brief RAII guard to safely remove temporary files created for standard input streams.
     */
    struct FTempInputGuard
    {
        std::optional<std::filesystem::path> Path;

        FTempInputGuard () = default;
        ~FTempInputGuard ();

        FTempInputGuard ( const FTempInputGuard & )           = delete;
        FTempInputGuard &operator=( const FTempInputGuard & ) = delete;
        FTempInputGuard ( FTempInputGuard &&Other ) noexcept;
        FTempInputGuard &operator=( FTempInputGuard &&Other ) noexcept;
    };

    /**
     * @struct FInputResolution
     * @brief Result of input resolution (resolved path + temp guard lifetime).
     */
    struct FInputResolution
    {
        std::string InputPath;
        FTempInputGuard TempGuard;
        bool bIsStdin = false;
    };

    /**
     * @struct FInputResolveOptions
     * @brief Resolution parameters (manifest discovery, command context name).
     */
    struct FInputResolveOptions
    {
        bool bAllowManifestFallback = false;
        std::string_view CommandName;
    };

    /**
     * @brief Returns standard -i/--input and --stdin CLI option definitions.
     */
    [[nodiscard]] std::vector<FOption> GetInputOptions ( FInputFlags &Flags,
                                                         std::string_view Description = "File input source program" );

    /**
     * @brief Resolves source input from stdin, positional arguments, explicit flags, or project manifest fallback.
     * @return Resolved input result on success, or std::nullopt on failure (errors logged automatically).
     */
    [[nodiscard]] std::optional<FInputResolution> ResolveInput ( const FInputFlags &Flags,
                                                                 const CommandParser::FParsedArgs &ParsedArgs,
                                                                 const FInputResolveOptions &Options = {} );

} // namespace CLI

} // namespace Volt
