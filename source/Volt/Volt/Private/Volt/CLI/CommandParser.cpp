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
                                                                         std::span<const FOption> InOptions,
                                                                         const std::size_t InMaxPositionals )
{
    FParsedArgs Parsed;
    const std::size_t ArgCount = InArgs.size();
    bool bParsingOptions       = true;

    for ( std::size_t Idx = 0; Idx < ArgCount; ++Idx )
    {
        const std::string_view Arg = InArgs[Idx];

        if ( bParsingOptions and Arg == "--" )
        {
            bParsingOptions = false;
            continue;
        }

        if ( bParsingOptions and ( Arg == "-h" or Arg == "--help" ) )
        {
            Parsed.bHelpRequested = true;
        }
        else if ( bParsingOptions and Arg.starts_with( '-' ) )
        {
            std::string_view OptName = Arg;
            std::string_view InlineValue;
            bool bHasInlineValue = false;

            const std::size_t EqPos = Arg.find( '=' );
            if ( EqPos != std::string_view::npos )
            {
                OptName         = Arg.substr( 0, EqPos );
                InlineValue     = Arg.substr( EqPos + 1 );
                bHasInlineValue = true;
            }

            const FOption *Opt = FindOption( InOptions, OptName );
            if ( Opt == nullptr )
            {
                return MakeUnknownOptionError( std::string( OptName ) );
            }

            if ( Opt->HasValue() )
            {
                if ( bHasInlineValue )
                {
                    Opt->Callback( InlineValue );
                }
                else if ( Idx + 1 < ArgCount )
                {
                    const std::string_view NextArg = InArgs[Idx + 1];
                    if ( NextArg.starts_with( '-' ) )
                    {
                        return MakeMissingValueError( std::string( OptName ) );
                    }
                    ++Idx;
                    Opt->Callback( NextArg );
                }
                else
                {
                    return MakeMissingValueError( std::string( OptName ) );
                }
            }
            else
            {
                if ( bHasInlineValue )
                {
                    return MakeUnexpected( "Option '" + std::string( OptName ) + "' does not take a value" );
                }
                Opt->Callback( "" );
            }
        }
        else
        {
            if ( Parsed.Positionals.size() >= InMaxPositionals )
            {
                return MakeUnexpected( "Unexpected argument: " + std::string( Arg ) );
            }
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
