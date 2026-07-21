#include "Volt/CLI/Commands/VersionCommand.hpp"
#include "Volt/CLI/CommandColor.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/Version.hpp"
#include "Volt/Core/Log/Logger.hpp"

#include <print>
#include <string_view>

/**
 * Public
 */

std::string_view Volt::CLI::FVersionCommand::GetName () const noexcept
{
    return "version";
}

std::string_view Volt::CLI::FVersionCommand::GetDescription () const noexcept
{
    return "Display the version information";
}

std::string_view Volt::CLI::FVersionCommand::GetUsage () const noexcept
{
    return "volt version";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FVersionCommand::GetOptions ()
{
    return {};
}

std::int32_t Volt::CLI::FVersionCommand::Execute ( std::span<const std::string_view> InArgs )
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

    Core::FLogger::Flush();
    std::println( "{}\n{} {} (0x{:06x})", Paint( AnsiYellowBold, "Volt Language", bColor ),
                  Paint( AnsiGreen, "Version: ", bColor ), Paint( AnsiYellowBold, VoltVersion, bColor ), CombinedVersion );

    return ExitSuccess;
}

/**
 * Private
 */

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FVersionCommand> RegisterVersionCommand;

} // namespace
