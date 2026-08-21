#include "Volt/CLI/Commands/ReplCommand.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/StdlibCache.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/ReplCore/LineState.hpp"
#include "Volt/ReplEval/Evaluator.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

constexpr std::string_view Prompt         = "volt> ";
constexpr std::string_view ContinuePrompt = "    | ";

[[nodiscard]] bool StandardInputIsATerminal ()
{
    return isatty( STDIN_FILENO ) == 1;
}

} // namespace

std::string_view Volt::CLI::FReplCommand::GetName () const noexcept
{
    return "repl";
}

std::string_view Volt::CLI::FReplCommand::GetDescription () const noexcept
{
    return "Start an interactive session";
}

std::string_view Volt::CLI::FReplCommand::GetUsage () const noexcept
{
    return "volt repl [options]";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FReplCommand::GetOptions ()
{
    // clang-format off
    std::vector<FOption> Options = {
        {
            "-O", "", "LEVEL", "Optimization level (0|1|2|3, default 0)",
            [this] ( std::string_view Val ) { this->OptLevel = Val; }
        },
        {
            "-e", "--eval", "EXPR", "Evaluate one line and exit (repeatable)",
            [this] ( std::string_view Val ) { this->EvalLines.emplace_back( Val ); }
        },
        {
            "-v", "--verbose", "", "Enable verbose output",
            [this] ( std::string_view ) { this->bVerbose = true; this->StdlibFlags.bVerbose = true; }
        }
    };
    // clang-format on

    for ( FOption &Option : StdlibCacheOptions( StdlibFlags ) )
    {
        Options.push_back( std::move( Option ) );
    }
    return Options;
}

std::int32_t Volt::CLI::FReplCommand::Execute ( std::span<const std::string_view> InArgs )
{
    const std::vector<FOption> Options = GetOptions();
    const auto Parsed                  = CommandParser::Parse( InArgs, Options );
    if ( not Parsed.has_value() )
    {
        Core::FLogger::Error( Parsed.error() );
        Core::FLogger::Flush();
        CommandParser::PrintUsage( std::cerr, GetUsage(), Options );
        return ExitFailure;
    }
    if ( Parsed->bHelpRequested )
    {
        Core::FLogger::Flush();
        CommandParser::PrintUsage( std::cout, GetUsage(), Options );
        return ExitSuccess;
    }

    Repl::EvaluatorOptions EvalOpts;
    EvalOpts.OptLevel  = OptLevel.empty() ? std::uint8_t{ 0 } : static_cast<std::uint8_t>( OptLevel[0] - '0' );
    EvalOpts.bNoCache  = StdlibFlags.bNoStdlibCache;
    EvalOpts.bFresh    = StdlibFlags.bFreshStdlib;
    EvalOpts.bNoStdlib = StdlibFlags.bNoStdlib;
    EvalOpts.bVerbose  = bVerbose;

    Repl::Evaluator Session;
    std::string StartError;
    if ( not Session.Start( EvalOpts, StartError ) )
    {
        Core::FLogger::Error( StartError, "repl" );
        Core::FLogger::Flush();
        return ExitFailure;
    }

    const bool bInteractive = StandardInputIsATerminal() and EvalLines.empty();

    // One place that turns a finished statement into output, so `-e`, a piped
    // script and a live prompt cannot disagree on what a result looks like.
    std::int32_t Status = ExitSuccess;
    const auto Evaluate = [&] ( const std::string &Statement )
    {
        const Repl::EvalOutcome Outcome = Session.Feed( Statement );

        std::cout << Outcome.Diagnostics;
        if ( not Outcome.Message.empty() )
        {
            std::cout << Outcome.Message << '\n';
        }

        // The value half of `=> 3 : Int32` was written by the evaluated code
        // itself, straight to the descriptor, before Feed returned. Flushing
        // first is what keeps the two halves in the order they are read: this
        // stream is buffered and that one is not.
        // Only when the value actually rendered. A line whose result has no
        // `to_string` says nothing at all rather than announcing a type with no
        // value beside it — `puts( x )` is a side effect the user came for, and
        // a trailing `=> <StandardStream>` is noise. `:type` is where a type is
        // asked for on purpose.
        if ( Outcome.bRendered )
        {
            std::cout.flush();
            std::cout << " : " << Outcome.ResultType << '\n';
        }
        std::cout.flush();

        if ( Outcome.Status != Repl::EEvalStatus::Ok )
        {
            // A bad line is not a bad session, so the loop goes on; but a
            // non-interactive run has nobody to correct it, and reporting
            // success would make a broken script look green in CI.
            Status = bInteractive ? Status : ExitFailure;
        }
    };

    for ( const std::string &Line : EvalLines )
    {
        Evaluate( Line );
    }
    if ( not EvalLines.empty() )
    {
        return Status;
    }

    if ( bInteractive )
    {
        std::cout << "volt repl -- ^D to leave\n";
    }

    // Accumulated across however many physical lines one statement spans.
    std::string Statement;
    std::string Line;

    while ( true )
    {
        if ( bInteractive )
        {
            std::cout << ( Statement.empty() ? Prompt : ContinuePrompt );
            std::cout.flush();
        }

        if ( not std::getline( std::cin, Line ) )
        {
            break;
        }

        Statement += Line;
        Statement += '\n';

        if ( Repl::Classify( Statement ) == Repl::ELineState::NeedsMore )
        {
            continue;
        }

        Evaluate( Statement );
        Statement.clear();
    }

    // Whatever was left when input ended is a statement the user meant to
    // finish. Evaluating it reports the syntax error rather than dropping it.
    if ( not Statement.empty() )
    {
        Evaluate( Statement );
    }

    if ( bInteractive )
    {
        std::cout << '\n';
    }
    return Status;
}

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FReplCommand> RegisterReplCommand;

} // namespace
