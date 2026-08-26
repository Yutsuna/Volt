#include "Volt/CLI/Commands/BuildCommand.hpp"
#include "Volt/CLI/CommandInputs.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/StdlibCache.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Core/Support/PhaseTimer.hpp"
#include "Volt/Driver/Driver.hpp"
#include "Volt/Frontend/AST/AstDump.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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
            "-O", "", "LEVEL", "Optimization level (0|1|2|3, default 2)",
            [this] ( std::string_view Val ) { this->OptLevel = Val; }
        },
        {
            "", "--emit", "KIND", "Stop after an intermediate artifact (ir|obj)",
            [this] ( std::string_view Val )
            {
                std::string Lower( Val );
                std::transform( Lower.begin(), Lower.end(), Lower.begin(),
                                [] ( unsigned char C ) { return static_cast<char>( std::tolower( C ) ); } );
                this->Emit = std::move( Lower );
            }
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

    // `build` is the only command that reaches the backend, so it is the only
    // place backend.* seams are ever recorded. Enabled before the Driver is
    // constructed: its constructor hashes the stdlib for the cache key, and
    // that is part of what a build costs.
    Core::PhaseTimings::SetEnabled( bVerbose );

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
        else if ( OptLevel == "1" )
        {
            BuildOpts.OptLevel = 1;
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
            Core::FLogger::Error( "Unsupported -O '" + OptLevel + "': expected 0, 1, 2 or 3", "build" );
            return ExitFailure;
        }
    }

    if ( Emit == "ast" or Emit == "resolved" )
    {
        const Frontend::FAstDumpOptions DumpOptions{
            .bColor        = Output.empty() and Core::FLogger::StdOutIsTerminal(),
            .bShowLocation = true,
        };
        if ( not Output.empty() )
        {
            std::ofstream File( Output );
            if ( not File )
            {
                Core::FLogger::Error( "Cannot write '" + Output + "'", "build" );
                return ExitFailure;
            }
            TheDriver.DumpUnits( File, DumpOptions, /*bUserUnitsOnly=*/true );
            return ExitSuccess;
        }
        std::ostringstream Buffer;
        TheDriver.DumpUnits( Buffer, DumpOptions, /*bUserUnitsOnly=*/true );
        Core::FLogger::Flush();
        std::cout << Buffer.str() << std::flush;
        return ExitSuccess;
    }

    const Driver::BuildResult Built = TheDriver.Build( BuildOpts );
    if ( not Built.bOk )
    {
        Core::FLogger::Error( Built.Message, "build" );
        return ExitFailure;
    }

    Core::FLogger::Info( "OK : " + Built.Artifact, "build" );

    if ( const std::vector<std::string> Lines = Core::FormatPhaseTimings(); not Lines.empty() )
    {
        Core::FLogger::Info( "timings:", "build" );
        for ( const std::string &Line : Lines )
        {
            Core::FLogger::Info( Line, "build" );
        }
    }
    return ExitSuccess;
}

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FBuildCommand> RegisterBuildCommand;

} // namespace
