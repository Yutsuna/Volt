#include "Volt/CLI/Commands/CheckCommand.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Driver/Driver.hpp"
#include "Volt/Driver/WellKnown.hpp"

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

/**
 * Private helpers
 */

namespace
{

namespace fs = std::filesystem;

/// One line per non-zero counter, named by the field itself. Nothing here
/// enumerates the counters: `--metrics` reports whatever PassStats declares,
/// so a pass that adds one is visible without touching the CLI. This is the
/// channel the AST-contract counters (ResidualSugarNodes, UntypedValueExprs)
/// are read through — they are hard errors as well, but a count is what makes
/// a regression measurable rather than just loud.
void ReportMetrics ( const Volt::Sema::PassStats &Stats )
{
    Volt::Core::FLogger::Info( "metrics:", "check" );
    Volt::Meta::ForEachField( Stats,
                              [] ( std::string_view Name, const std::size_t &Value )
                              {
                                  if ( Value > 0 )
                                  {
                                      Volt::Core::FLogger::Info( "  " + std::string{ Name } + " = " + std::to_string( Value ),
                                                                 "check" );
                                  }
                              } );
}

/// Walk up from InPath (file or directory) looking for a Project.vl manifest.
/// Mirrors the historical `Circuit.discover`: a file inside a circuit project
/// selects the whole circuit.
[[nodiscard]] std::optional<fs::path> DiscoverManifest ( const fs::path &InPath )
{
    std::error_code Ec;
    fs::path Dir = fs::absolute( fs::is_directory( InPath, Ec ) ? InPath : InPath.parent_path(), Ec );

    while ( !Dir.empty() )
    {
        fs::path Candidate = Dir / Volt::Driver::WellKnown::ManifestName;
        if ( fs::is_regular_file( Candidate, Ec ) )
        {
            return Candidate;
        }
        const fs::path Parent = Dir.parent_path();
        if ( Parent == Dir )
        {
            break;
        }
        Dir = Parent;
    }
    return std::nullopt;
}

} // namespace

/**
 * Public
 */

std::string_view Volt::CLI::FCheckCommand::GetName () const noexcept
{
    return "check";
}

std::string_view Volt::CLI::FCheckCommand::GetDescription () const noexcept
{
    return "Static semantic code analysis";
}

std::string_view Volt::CLI::FCheckCommand::GetUsage () const noexcept
{
    return "volt check [options] [input_file_or_dir]";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FCheckCommand::GetOptions ()
{
    // clang-format off
    return {
        {
            "-i", "--input", "INPUT", "Code target directory or source file",
            [this] ( std::string_view Val ) { this->Input = Val; }
        },
        {
            "", "--type", "TYPE", "Type verification scope (syntax|semantic|style)",
            [this] ( std::string_view Val ) { this->Type = Val; }
        },
        {
            "", "--rules", "RULES", "Location path defining validation rule models",
            [this] ( std::string_view Val ) { this->Rules = Val; }
        },
        {
            "", "--output", "OUT", "Output validation layout formats",
            [this] ( std::string_view Val ) { this->OutputFormat = Val; }
        },
        {
            "", "--warn-as-error", "", "Style warning events will promote to runtime errors",
            [this] ( std::string_view ) { this->bWarnAsError = true; }
        },
        {
            "", "--metrics", "", "Gather code statistics and size metric benchmarks",
            [this] ( std::string_view ) { this->bMetrics = true; }
        },
        {
            "", "--unused", "", "Flag unreferenced syntax structures and bindings",
            [this] ( std::string_view ) { this->bUnused = true; }
        }
    };
    // clang-format on
}

std::int32_t Volt::CLI::FCheckCommand::Execute ( std::span<const std::string_view> InArgs )
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

    if ( Input.empty() and not Result->Positionals.empty() )
    {
        Input = Result->Positionals.front();
    }
    else if ( not Input.empty() and not Result->Positionals.empty() )
    {
        Core::FLogger::Error( "Unexpected argument: " + std::string( Result->Positionals.front() ), "check" );
        Core::FLogger::Flush();
        CommandParser::PrintUsage( std::cerr, GetUsage(), Options );
        return ExitFailure;
    }
    if ( Input.empty() )
    {
        // Bare `volt check` inside a project directory checks that project.
        std::error_code Ec;
        if ( fs::is_regular_file( fs::current_path( Ec ) / Driver::WellKnown::ManifestName, Ec ) )
        {
            Input = fs::current_path( Ec ).string();
        }
    }
    if ( Input.empty() )
    {
        Core::FLogger::Error( "Missing source analyzer validation inputs (-i)", "check" );
        return ExitFailure;
    }

    std::error_code Ec;
    if ( not fs::exists( Input, Ec ) )
    {
        Core::FLogger::Error( "Cannot read '" + Input + "': no such file or directory", "check" );
        return ExitFailure;
    }

    Driver::Driver TheDriver;
    Driver::CompileResult Compiled;

    if ( const std::optional<fs::path> Manifest = DiscoverManifest( Input ) )
    {
        Core::FLogger::Info( "Circuit manifest found: " + Manifest->string(), "check" );
        Core::FLogger::Progress( "Running semantic analysis...", "check" );
        Compiled = TheDriver.CompileCircuit( Manifest->string() );
    }
    else
    {
        Core::FLogger::Progress( "Running semantic analysis...", "check" );
        Compiled = TheDriver.CompileFiles( { Input } );
    }

    const bool bFailed = TheDriver.HasErrors() or ( bWarnAsError and TheDriver.DiagnosticCount() > 0 );
    Core::FLogger::Progress( bFailed ? "Semantic analysis failed" : "Semantic analysis complete", "check", /*bFinished=*/true );

    if ( TheDriver.DiagnosticCount() > 0 )
    {
        Core::FLogger::Flush();
        TheDriver.RenderDiagnostics( std::cerr );
    }

    if ( bFailed )
    {
        Core::FLogger::Error(
            std::to_string( Compiled.Errors ) + " error(s) across " + std::to_string( Compiled.Files ) + " file(s)", "check" );
        return ExitFailure;
    }

    // Success front: one compact OK line, circuit-aware.
    std::string Summary = "OK : " + std::to_string( Compiled.Files ) + " file(s) type-checked";
    if ( TheDriver.Interfaces().Size() > 0 )
    {
        Summary += ", " + std::to_string( TheDriver.Interfaces().Size() ) + " exported decl(s)";
    }
    if ( Compiled.Stats.JsxLowered > 0 )
    {
        Summary += ", " + std::to_string( Compiled.Stats.JsxLowered ) + " jsx node(s) lowered";
    }
    if ( Compiled.Stats.PipelinesLowered > 0 )
    {
        Summary += ", " + std::to_string( Compiled.Stats.PipelinesLowered ) + " pipeline operator(s) lowered";
    }
    if ( TheDriver.Graph().ModuleCount() > 0 )

    {
        Summary += ", " + std::to_string( TheDriver.Graph().ModuleCount() ) + " module(s) in circuit `" +
                   TheDriver.Graph().NameOf( 0 ) + "`";
    }
    Core::FLogger::Info( Summary, "check" );

    if ( bMetrics )
    {
        ReportMetrics( Compiled.Stats );
    }
    return ExitSuccess;
}

/**
 * Private
 */

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FCheckCommand> RegisterCheckCommand;

} // namespace
