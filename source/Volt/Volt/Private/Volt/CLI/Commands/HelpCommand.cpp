#include "Volt/CLI/Commands/HelpCommand.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/Core/Log/Logger.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

/**
 * Private helpers
 */

namespace
{

constexpr std::string_view AnsiReset      = "\x1b[0m";
constexpr std::string_view AnsiYellowBold = "\x1b[1;33m";
constexpr std::string_view AnsiGreen      = "\x1b[32m";

[[nodiscard]] std::string Paint ( std::string_view Code, std::string_view Text, bool bColor )
{
    if ( !bColor )
    {
        return std::string( Text );
    }
    return std::string( Code ) + std::string( Text ) + std::string( AnsiReset );
}

} // namespace

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
    static_cast<void>( InArgs );

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
