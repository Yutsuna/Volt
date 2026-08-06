#include "Volt/CLI/Commands/ParseCommand.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/CommandInputs.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Driver/Driver.hpp"
#include "Volt/Frontend/AST/AstDump.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

std::string_view Volt::CLI::FParseCommand::GetName () const noexcept
{
    return "parse";
}

std::string_view Volt::CLI::FParseCommand::GetDescription () const noexcept
{
    return "Generate & display the abstract syntax tree";
}

std::string_view Volt::CLI::FParseCommand::GetUsage () const noexcept
{
    return "volt parse [options] [input_file]";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FParseCommand::GetOptions ()
{
    // clang-format off
    std::vector<FOption> Options = {
        {
            "-o", "--output", "OUTPUT", "Output target path structure",
            [this] ( std::string_view Val ) { this->Output = Val; }
        },
        {
            "", "--format", "FORMAT", "Serialization formats (json|dot|text)",
            [this] ( std::string_view Val ) { this->Format = Val; }
        },
        {
            "", "--simplify", "", "Deduplicate structural tree layout elements",
            [this] ( std::string_view ) { this->bSimplify = true; }
        },
        {
            "", "--lowered", "", "Display the tree after the AST lowering passes",
            [this] ( std::string_view ) { this->bLowered = true; }
        },
        {
            "", "--no-color", "", "Output without colors",
            [this] ( std::string_view ) { this->bNoColor = true; }
        },
        {
            "", "--no-location", "", "Omit character and index coordinates",
            [this] ( std::string_view ) { this->bNoLocation = true; }
        }
    };
    // clang-format on

    for ( FOption &Option : GetInputOptions( InputFlags, "Source input module path" ) )
    {
        Options.push_back( std::move( Option ) );
    }

    return Options;
}

std::int32_t Volt::CLI::FParseCommand::Execute ( std::span<const std::string_view> InArgs )
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

    const auto InputRes = ResolveInput( InputFlags, *Result, { .bAllowManifestFallback = false, .CommandName = "parse" } );
    if ( not InputRes.has_value() )
    {
        return ExitFailure;
    }

    const std::string &Input = InputRes->InputPath;

    Driver::Driver TheDriver;
    static_cast<void>( TheDriver.ParseFiles( { Input }, bLowered ) );

    if ( TheDriver.HasErrors() )
    {
        Core::FLogger::Flush();
        TheDriver.RenderDiagnostics( std::cerr );
        return ExitFailure;
    }

    const Frontend::FAstDumpOptions DumpOptions{
        .bColor        = not bNoColor and Output.empty() and Core::FLogger::StdOutIsTerminal(),
        .bShowLocation = not bNoLocation,
    };

    if ( not Output.empty() )
    {
        std::ofstream File( Output );
        if ( not File )
        {
            Core::FLogger::Error( "Cannot write '" + Output + "'" );
            return ExitFailure;
        }
        TheDriver.DumpUnits( File, DumpOptions );
        return ExitSuccess;
    }

    std::ostringstream Buffer;
    TheDriver.DumpUnits( Buffer, DumpOptions );
    Core::FLogger::Flush();
    std::cout << Buffer.str() << std::flush;
    return ExitSuccess;
}

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FParseCommand> RegisterParseCommand;

} // namespace
