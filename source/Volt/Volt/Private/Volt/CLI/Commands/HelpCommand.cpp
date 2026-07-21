#include "Volt/CLI/Commands/HelpCommand.hpp"
#include "Volt/CLI/CommandColor.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/Core/Log/Logger.hpp"

#include <iostream>
#include <string_view>

/**
 * Public
 */

std::string_view Volt::CLI::FHelpCommand::GetName () const noexcept
{
    return "help";
}

std::string_view Volt::CLI::FHelpCommand::GetDescription () const noexcept
{
    return "Display the usage";
}

std::string_view Volt::CLI::FHelpCommand::GetUsage () const noexcept
{
    return "volt help";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FHelpCommand::GetOptions ()
{
    return {};
}

std::int32_t Volt::CLI::FHelpCommand::Execute ( std::span<const std::string_view> InArgs )
{
    const std::vector<FOption> Options = GetOptions();
    const auto Result                  = CommandParser::Parse( InArgs, Options, 0 );
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

    const bool bColor = Core::FLogger::StdOutIsTerminal();

    std::size_t Widest = 0;
    for ( const auto &[Name, Command] : FCommandRegistry::GetInstance().GetRegisteredCommands() )
    {
        Widest = std::max( Widest, Name.size() );
    }

    Core::FLogger::Flush();
    std::cout << Paint( AnsiYellowBold, "Volt Language", bColor ) << '\n';
    std::cout << "Usage: volt <command> [options]\n";
    for ( const auto &[Name, Command] : FCommandRegistry::GetInstance().GetRegisteredCommands() )
    {
        const std::string Padded = std::string( Name ) + std::string( Widest - Name.size(), ' ' );
        std::cout << "  " << Paint( AnsiGreen, Padded, bColor ) << "  " << Command->GetDescription() << '\n';
    }
    std::cout << std::flush;
    return ExitSuccess;
}

/**
 * Private
 */

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FHelpCommand> RegisterHelpCommand;

} // namespace
