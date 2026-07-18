#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/GenericCommand.hpp"

#include <algorithm>
#include <expected>
#include <ostream>
#include <span>
#include <string>

/**
 * Private helpers
 */

namespace
{

inline std::unexpected<std::string> MakeUnexpected ( const std::string &InMessage )
{
    return std::unexpected( InMessage );
}

inline std::unexpected<std::string> MakeMissingValueError ( const std::string &InOption )
{
    return MakeUnexpected( "Missing value for option " + InOption );
}

inline std::unexpected<std::string> MakeUnknownOptionError ( const std::string &InOption )
{
    return MakeUnexpected( "Invalid option: " + InOption );
}

const Volt::CLI::FOption *FindOption ( const std::span<const Volt::CLI::FOption> InOptions,
                                       const std::string_view InName ) noexcept
{
    for ( const Volt::CLI::FOption &Opt : InOptions )
    {
        if ( InName == Opt.ShortName or InName == Opt.LongName )
        {
            return &Opt;
        }
    }
    return nullptr;
}

std::expected<void, std::string>
ValidateOptionValue ( const Volt::CLI::FOption &InOpt, const std::span<const std::string_view> InArgs, std::size_t &InOutIdx )
{
    if ( InOpt.HasValue() )
    {
        if ( InOutIdx + 1 < InArgs.size() )
        {
            ++InOutIdx;
            InOpt.Callback( InArgs[InOutIdx] );
        }
        else
        {
            return MakeMissingValueError( std::string( InOpt.LongName ) );
        }
    }
    else
    {
        InOpt.Callback( "" );
    }
    return {};
}

/// The flags column of one usage row: "-i INPUT, --input INPUT".
std::string FormatFlags ( const Volt::CLI::FOption &InOpt )
{
    std::string Flags;
    if ( !InOpt.ShortName.empty() )
    {
        Flags += InOpt.ShortName;
        if ( InOpt.HasValue() )
        {
            Flags += ' ';
            Flags += InOpt.ValueName;
        }
        Flags += ", ";
    }
    Flags += InOpt.LongName;
    if ( InOpt.HasValue() )
    {
        Flags += ' ';
        Flags += InOpt.ValueName;
    }
    return Flags;
}

} // namespace

/**
 * Public
 */

Volt::CLI::CommandParser::FParseResult Volt::CLI::CommandParser::Parse ( std::span<const std::string_view> InArgs,
                                                                         std::span<const FOption> InOptions )
{
    FParsedArgs Parsed;
    const std::size_t ArgCount = InArgs.size();

    for ( std::size_t Idx = 0; Idx < ArgCount; ++Idx )
    {
        const std::string_view Arg = InArgs[Idx];

        if ( Arg == "-h" or Arg == "--help" )
        {
            Parsed.bHelpRequested = true;
        }
        else if ( Arg.starts_with( '-' ) )
        {
            const FOption *Opt = FindOption( InOptions, Arg );
            if ( Opt == nullptr )
            {
                return MakeUnknownOptionError( std::string( Arg ) );
            }
            const auto Result = ValidateOptionValue( *Opt, InArgs, Idx );
            if ( not Result )
            {
                return MakeUnexpected( Result.error() );
            }
        }
        else
        {
            Parsed.Positionals.push_back( Arg );
        }
    }
    return Parsed;
}

void Volt::CLI::CommandParser::PrintUsage ( std::ostream &Out, std::string_view InUsage, std::span<const FOption> InOptions )
{
    Out << "Usage: " << InUsage << '\n';

    std::vector<std::string> Rows;
    Rows.reserve( InOptions.size() + 1 );
    std::size_t Widest = 0;
    for ( const FOption &Opt : InOptions )
    {
        Rows.push_back( FormatFlags( Opt ) );
        Widest = std::max( Widest, Rows.back().size() );
    }
    const std::string HelpFlags = "-h, --help";
    Widest                      = std::max( Widest, HelpFlags.size() );

    for ( std::size_t Idx = 0; Idx < InOptions.size(); ++Idx )
    {
        Out << "    " << Rows[Idx] << std::string( Widest - Rows[Idx].size() + 4, ' ' ) << InOptions[Idx].Description << '\n';
    }
    Out << "    " << HelpFlags << std::string( Widest - HelpFlags.size() + 4, ' ' ) << "Show help" << '\n';
}
