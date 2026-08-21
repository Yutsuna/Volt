#include "Volt/CLI/Commands/RunCommand.hpp"
#include "Volt/CLI/CommandInputs.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/StdlibCache.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Core/Support/PhaseTimer.hpp"
#include "Volt/Driver/Driver.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

std::string_view Volt::CLI::FRunCommand::GetName () const noexcept
{
    return "run";
}

std::string_view Volt::CLI::FRunCommand::GetDescription () const noexcept
{
    return "Compile and run the file input";
}

std::string_view Volt::CLI::FRunCommand::GetUsage () const noexcept
{
    return "volt run [options] [input_file] [-- program_args...]";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FRunCommand::GetOptions ()
{
    // clang-format off
    std::vector<FOption> Options = {
        {
            "", "--target", "TARGET", "Code generation target (native)",
            [this] ( std::string_view Val ) { this->Target = Val; }
        },
        {
            "-O", "", "LEVEL", "Optimization level (0|1|2|3, default 0)",
            [this] ( std::string_view Val ) { this->OptLevel = Val; }
        },
        {
            "", "--load", "PATH", "Resolve symbols against a shared object first",
            [this] ( std::string_view Val ) { this->Dylibs.emplace_back( Val ); }
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

std::int32_t Volt::CLI::FRunCommand::Execute ( std::span<const std::string_view> InArgs )
{
    // Everything after a bare `--` belongs to the program, not to volt. Split
    // before parsing so a program's own `-O` or `--target` is never mistaken
    // for one of ours.
    std::vector<std::string_view> OwnArgs;
    std::vector<std::string> ProgramArgs;
    bool bSeenSeparator = false;
    for ( const std::string_view Arg : InArgs )
    {
        if ( not bSeenSeparator and Arg == "--" )
        {
            bSeenSeparator = true;
            continue;
        }
        if ( bSeenSeparator )
        {
            ProgramArgs.emplace_back( Arg );
        }
        else
        {
            OwnArgs.push_back( Arg );
        }
    }

    const std::vector<FOption> Options = GetOptions();
    const auto Result                  = CommandParser::Parse( OwnArgs, Options );
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

    const auto InputRes = ResolveInput( InputFlags, *Result, { .bAllowManifestFallback = true, .CommandName = "run" } );
    if ( not InputRes.has_value() )
    {
        return ExitFailure;
    }

    const std::string &Input = InputRes->InputPath;

    Core::PhaseTimings::SetEnabled( bVerbose );

    Driver::Driver TheDriver;
    Driver::CompileResult Compiled;

    const Driver::FCacheOptions CacheOpts = ToDriverCacheOptions( StdlibFlags );

    if ( const std::optional<fs::path> Manifest = Driver::DiscoverManifest( Input ) )
    {
        Core::FLogger::Info( "Circuit manifest found: " + Manifest->string(), "run" );
        Core::FLogger::Progress( "Compiling...", "run" );
        Compiled = TheDriver.CompileCircuit( Manifest->string(), CacheOpts );
    }
    else
    {
        Core::FLogger::Progress( "Compiling...", "run" );
        Compiled = TheDriver.CompileFiles( { Input }, CacheOpts );
    }

    Core::FLogger::Progress( TheDriver.HasErrors() ? "Compilation failed" : "Compilation complete", "run",
                             /*bFinished=*/true );

    if ( TheDriver.DiagnosticCount() > 0 )
    {
        Core::FLogger::Flush();
        TheDriver.RenderDiagnostics( std::cerr );
    }

    if ( TheDriver.HasErrors() )
    {
        Core::FLogger::Error(
            std::to_string( Compiled.Errors ) + " error(s) across " + std::to_string( Compiled.Files ) + " file(s)", "run" );
        return ExitFailure;
    }

    Driver::RunOptions RunOpts;
    RunOpts.Target   = Target;
    RunOpts.Dylibs   = Dylibs;
    RunOpts.bVerbose = bVerbose;

    // argv[0] is the program's own name, as it would be under exec.
    RunOpts.ProgramArgs.reserve( ProgramArgs.size() + 1 );
    RunOpts.ProgramArgs.push_back( Input );
    for ( std::string &Arg : ProgramArgs )
    {
        RunOpts.ProgramArgs.push_back( std::move( Arg ) );
    }

    if ( not OptLevel.empty() )
    {
        if ( OptLevel == "0" or OptLevel == "1" or OptLevel == "2" or OptLevel == "3" )
        {
            RunOpts.OptLevel = static_cast<std::uint8_t>( OptLevel[0] - '0' );
        }
        else
        {
            Core::FLogger::Error( "Unsupported -O '" + OptLevel + "': expected 0, 1, 2 or 3", "run" );
            return ExitFailure;
        }
    }

    Core::FLogger::Flush();

    const Driver::RunResult Ran = TheDriver.Run( RunOpts );
    if ( not Ran.bOk )
    {
        Core::FLogger::Error( Ran.Message, "run" );
        return ExitFailure;
    }

    if ( const std::vector<std::string> Lines = Core::FormatPhaseTimings(); not Lines.empty() )
    {
        Core::FLogger::Info( "timings:", "run" );
        for ( const std::string &Line : Lines )
        {
            Core::FLogger::Info( Line, "run" );
        }
    }

    // The program's exit code is this command's exit code, so `volt run`
    // composes in a shell exactly as running the built artifact would.
    return Ran.Code;
}

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FRunCommand> RegisterRunCommand;

} // namespace
