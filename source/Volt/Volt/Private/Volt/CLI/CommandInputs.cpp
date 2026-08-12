#include "Volt/CLI/CommandInputs.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Driver/WellKnown.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

Volt::CLI::FTempInputGuard::~FTempInputGuard ()
{
    if ( Path )
    {
        std::error_code Ec;
        fs::remove( *Path, Ec );
        if ( Ec )
        {
            Core::FLogger::Warn( "Failed to remove temporary input file '" + Path->string() + "': " + Ec.message() );
        }
    }
}

Volt::CLI::FTempInputGuard::FTempInputGuard ( FTempInputGuard &&Other ) noexcept : Path( std::move( Other.Path ) )
{
    Other.Path.reset();
}

Volt::CLI::FTempInputGuard &Volt::CLI::FTempInputGuard::operator=( FTempInputGuard &&Other ) noexcept
{
    if ( this != &Other )
    {
        if ( Path )
        {
            std::error_code Ec;
            fs::remove( *Path, Ec );
        }
        Path = std::move( Other.Path );
        Other.Path.reset();
    }
    return *this;
}

std::vector<Volt::CLI::FOption> Volt::CLI::GetInputOptions ( FInputFlags &Flags, std::string_view Description )
{
    // clang-format off
    return {
        {
            "-i", "--input", "INPUT", Description,
            [&Flags] ( std::string_view Val ) { Flags.ExplicitInput = Val; }
        },
        {
            "", "--stdin", "", "Read source from standard input",
            [&Flags] ( std::string_view ) { Flags.bStdin = true; }
        }
    };
    // clang-format on
}

std::optional<Volt::CLI::FInputResolution> Volt::CLI::ResolveInput ( const FInputFlags &Flags,
                                                                     const CommandParser::FParsedArgs &ParsedArgs,
                                                                     const FInputResolveOptions &Options )
{
    const std::string_view LogTag = Options.CommandName;

    if ( Flags.bStdin )
    {
        if ( not Flags.ExplicitInput.empty() or not ParsedArgs.Positionals.empty() )
        {
            Core::FLogger::Error( "--stdin cannot be combined with input files", std::string( LogTag ) );
            return std::nullopt;
        }

        std::ostringstream SourceStream;
        SourceStream << std::cin.rdbuf();
        const std::string Source = SourceStream.str();

        if ( Source.empty() )
        {
            Core::FLogger::Error( "No data provided on standard input", std::string( LogTag ) );
            return std::nullopt;
        }

        std::error_code Ec;
        const fs::path TempDir = fs::temp_directory_path( Ec );
        if ( Ec )
        {
            Core::FLogger::Error( "Cannot access temporary directory: " + Ec.message(), std::string( LogTag ) );
            return std::nullopt;
        }

        const fs::path TempPath = TempDir / "volt_stdin.vl";
        {
            std::ofstream Out( TempPath, std::ios::binary );
            if ( not Out )
            {
                Core::FLogger::Error( "Cannot create temporary file for stdin input", std::string( LogTag ) );
                return std::nullopt;
            }
            Out.write( Source.data(), static_cast<std::streamsize>( Source.size() ) );
        }

        FTempInputGuard Guard;
        Guard.Path = TempPath;

        return FInputResolution{
            .InputPath = TempPath.string(),
            .TempGuard = std::move( Guard ),
            .bIsStdin  = true,
        };
    }

    std::string Input = Flags.ExplicitInput;

    if ( Input.empty() and not ParsedArgs.Positionals.empty() )
    {
        Input = std::string( ParsedArgs.Positionals.front() );
    }
    else if ( not Input.empty() and not ParsedArgs.Positionals.empty() )
    {
        Core::FLogger::Error( "Unexpected argument: " + std::string( ParsedArgs.Positionals.front() ), std::string( LogTag ) );
        return std::nullopt;
    }

    if ( Input.empty() and Options.bAllowManifestFallback )
    {
        std::error_code Ec;
        if ( fs::is_regular_file( fs::current_path( Ec ) / Driver::WellKnown::ManifestName, Ec ) )
        {
            Input = fs::current_path( Ec ).string();
        }
    }

    if ( Input.empty() )
    {
        Core::FLogger::Error( "Missing source input (-i)", std::string( LogTag ) );
        return std::nullopt;
    }

    std::error_code Ec;
    if ( not fs::exists( Input, Ec ) )
    {
        Core::FLogger::Error( "Cannot read '" + Input + "': no such file or directory", std::string( LogTag ) );
        return std::nullopt;
    }

    return FInputResolution{
        .InputPath = Input,
        .TempGuard = {},
        .bIsStdin  = false,
    };
}
