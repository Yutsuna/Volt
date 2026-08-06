#include "Volt/CLI/Commands/BuildCommand.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/CommandInputs.hpp"
#include "Volt/CLI/StdlibCache.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Driver/Driver.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

std::string_view Volt::CLI::FBuildCommand::GetName () const noexcept
{
    return "build";
}

std::string_view Volt::CLI::FBuildCommand::GetDescription () const noexcept
{
    return "Compile the file input";
}

std::string_view Volt::CLI::FBuildCommand::GetUsage () const noexcept
{
    return "volt build [options] [input_file]";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FBuildCommand::GetOptions ()
{
    // clang-format off
    std::vector<FOption> Options = {
        {
            "-o", "--output", "OUTPUT", "Output artifact path",
            [this] ( std::string_view Val ) { this->Output = Val; }
        },
        {
            "", "--target", "TARGET", "Code generation target (native|wasm)",
            [this] ( std::string_view Val ) { this->Target = Val; }
        },
        {
            "-O", "", "LEVEL", "Optimization level (0|2|3)",
            [this] ( std::string_view Val ) { this->OptLevel = Val; }
        },
        {
            "", "--emit", "KIND", "Stop after an intermediate artifact (ir|obj)",
            [this] ( std::string_view Val ) { this->Emit = Val; }
        },
        {
            "", "--lto", "", "Enable link-time optimization (native only)",
            [this] ( std::string_view ) { this->bLto = true; }
        },
        {
            "-v", "--verbose", "", "Enable verbose output",
            [this] ( std::string_view ) { this->bVerbose = true; this->StdlibFlags.bVerbose = true; }
        }
    };
    // clang-format on

    for ( FOption &Option : GetInputOptions( InputFlags, "File input source program" ) )
    {
        Options.push_back( std::move( Option ) );
    }

    for ( FOption &Option : StdlibCacheOptions( StdlibFlags ) )
    {
        Options.push_back( std::move( Option ) );
    }
    return Options;
}

std::int32_t Volt::CLI::FBuildCommand::Execute ( std::span<const std::string_view> InArgs )
{
    const std::vector<FOption> Options = GetOptions();
    const auto Result                  = CommandParser::Parse( InArgs, Options );
    if ( not Result.has_value() )
    {
        Core::FLogger::Error( Result.error() );
        Core::FLogger::Flush();
        CommandParser::PrintUsage( std::cerr, GetUsage(), Options );
        return ExitFailure;
    }
    if ( Result->bHelpRequested )
    {
        Core::FLogger::Flush();
        CommandParser::PrintUsage( std::cout, GetUsage(), Options );
        return ExitSuccess;
    }

    const auto InputRes = ResolveInput( InputFlags, *Result, { .bAllowManifestFallback = true, .CommandName = "build" } );
    if ( not InputRes.has_value() )
    {
        return ExitFailure;
    }

    const std::string &Input = InputRes->InputPath;

    Driver::Driver TheDriver;
    Driver::CompileResult Compiled;

    const Driver::FCacheOptions CacheOpts = ToDriverCacheOptions( StdlibFlags );

    if ( const std::optional<fs::path> Manifest = Driver::DiscoverManifest( Input ) )
    {
        Core::FLogger::Info( "Circuit manifest found: " + Manifest->string(), "build" );
        Core::FLogger::Progress( "Compiling...", "build" );
        Compiled = TheDriver.CompileCircuit( Manifest->string(), CacheOpts );
    }
    else
    {
        Core::FLogger::Progress( "Compiling...", "build" );
        Compiled = TheDriver.CompileFiles( { Input }, CacheOpts );
    }

    Core::FLogger::Progress( TheDriver.HasErrors() ? "Compilation failed" : "Compilation complete", "build",
                             /*bFinished=*/true );

    if ( TheDriver.DiagnosticCount() > 0 )
    {
        Core::FLogger::Flush();
        TheDriver.RenderDiagnostics( std::cerr );
    }

    if ( TheDriver.HasErrors() )
    {
        Core::FLogger::Error(
            std::to_string( Compiled.Errors ) + " error(s) across " + std::to_string( Compiled.Files ) + " file(s)", "build" );
        return ExitFailure;
    }

    Driver::BuildOptions BuildOpts;
    BuildOpts.Target                 = Target;
    BuildOpts.OutputPath             = Output;
    BuildOpts.bLto                   = bLto;
    BuildOpts.Emit                   = Emit;
    BuildOpts.bStdlibArtifactNoCache = StdlibFlags.bNoStdlibCache;
    BuildOpts.bStdlibArtifactFresh   = StdlibFlags.bFreshStdlib;
    BuildOpts.StdlibArtifactKind     = StdlibFlags.Artifact;
    BuildOpts.bVerbose               = bVerbose;

    if ( not OptLevel.empty() )
    {
        if ( OptLevel == "0" )
        {
            BuildOpts.OptLevel = 0;
        }
        else if ( OptLevel == "2" )
        {
            BuildOpts.OptLevel = 2;
        }
        else if ( OptLevel == "3" )
        {
            BuildOpts.OptLevel = 3;
        }
        else
        {
            Core::FLogger::Error( "Unsupported -O '" + OptLevel + "': expected 0, 2 or 3", "build" );
            return ExitFailure;
        }
    }

    const Driver::BuildResult Built = TheDriver.Build( BuildOpts );
    if ( not Built.bOk )
    {
        Core::FLogger::Error( Built.Message, "build" );
        return ExitFailure;
    }

    Core::FLogger::Info( "OK : " + Built.Artifact, "build" );
    return ExitSuccess;
}

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FBuildCommand> RegisterBuildCommand;

} // namespace
