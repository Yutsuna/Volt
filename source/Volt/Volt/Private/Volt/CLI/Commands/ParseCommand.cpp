#include "Volt/CLI/Commands/ParseCommand.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/GenericCommand.hpp"

/**
 * Public
 */

std::string_view Volt::CLI::FParseCommand::GetName () const noexcept
{
    return "parse";
}

std::string_view Volt::CLI::FParseCommand::GetDescription () const noexcept
{
    return "Parses the specified volt input and outputs the result in the specified format.";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FParseCommand::GetOptions ()
{
    // clang-format off
    return {
        {
            "-i", "--input", "Source input module path",
            true,
            [this] ( std::string_view Val ) { this->Input = Val; }
        },
        {
            "-o", "--output", "Output target path structure",
            true,
            [this] ( std::string_view Val ) { this->Output = Val; }
        },
        {
            "--format", "Serialization formats (json|dot|text)",
            true,
            [this] ( std::string_view Val ) { this->Format = Val; }
        },
        {
            "--simplify", "Deduplicate structural tree layout elements",
            false,
            [this] ( std::string_view ) { this->bSimplify = true; }
        },
        {
            "--no-color", "Output without colors",
            false,
            [this] ( std::string_view ) { this->bNoColor = true; }
        },
        {
            "--no-location", "Omit character and index coordinates",
            false,
            [this] ( std::string_view ) { this->bNoLocation = true; }
        },
        {
            "-h", "--help", "Show help",
            false,
            [] ( std::string_view ) {}
        }
    };
    // clang-format on
}

std::int32_t Volt::CLI::FParseCommand::Execute ( std::span<const std::string_view> InArgs )
{
    const auto Result = CommandParser::Parse( InArgs, GetOptions() );
    if ( not Result.has_value() )
    {
        // TODO: log
        return ExitFailure;
    }

    if ( Input.empty() and not Result->empty() )
    {
        Input = Result->front();
    }
    if ( Input.empty() )
    {
        // TODO: log
        return ExitFailure;
    }

    // TODO: do the rest
}

/**
 * Private
 */

static const Volt::CLI::TCommandRegister<Volt::CLI::FParseCommand> RegisterBuildCommand;
