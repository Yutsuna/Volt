#include "Volt/CLI/Commands/VersionCommand.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/Version.hpp"
#include "Volt/Core/Log/Logger.hpp"

#include <print>
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
    static_cast<void>( InArgs );

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
