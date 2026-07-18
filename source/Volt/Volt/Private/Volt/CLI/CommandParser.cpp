#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/GenericCommand.hpp"

#include <expected>
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
    return MakeUnexpected( "Unknown option: " + InOption );
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
    if ( InOpt.bHasValue )
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

} // namespace

/**
 * public
 */

Volt::CLI::CommandParser::FParseResult Volt::CLI::CommandParser::Parse ( std::span<const std::string_view> InArgs,
                                                                         std::span<const FOption> InOptions )
{
    std::vector<std::string_view> Positionals;
    const std::size_t ArgCount = InArgs.size();

    for ( std::size_t Idx = 0; Idx < ArgCount; ++Idx )
    {
        const std::string_view Arg = InArgs[Idx];

        if ( Arg.starts_with( '-' ) )
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
            Positionals.push_back( Arg );
        }
    }
    return Positionals;
}
