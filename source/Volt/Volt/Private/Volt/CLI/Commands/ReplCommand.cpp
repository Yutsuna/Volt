#include "Volt/CLI/Commands/ReplCommand.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/StdlibCache.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/ReplCore/LineState.hpp"
#include "Volt/ReplEval/Evaluator.hpp"
#include "Volt/ReplQuery/QueryEngine.hpp"
#include "Volt/ReplTui/Terminal.hpp"

#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"

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

    // A terminal gets the terminal front end: raw mode, colour, completion, a
    // side panel. Everything else — a pipe, a redirect, `-e` — takes the plain
    // path below, which writes no escape sequence anywhere and is the path the
    // test suite drives.
    if ( bInteractive and Repl::Tui::IsInteractiveTerminal() )
    {
        Core::FLogger::Flush();

        Repl::Tui::SessionOptions TuiOpts;
        TuiOpts.bColor      = true;
        TuiOpts.HistoryPath = Repl::Tui::DefaultHistoryPath();
        return Repl::Tui::Run( Session, TuiOpts );
    }

    // One place that turns a finished statement into output, so `-e`, a piped
    // script and a live prompt cannot disagree on what a result looks like.
    std::int32_t Status = ExitSuccess;

    // The builtins, without a terminal. `:type`, `:layout` and the rest are
    // questions about the session rather than a feature of the front end, so a
    // script can ask them — and the tests that pin their answers run here,
    // where there is no colour to strip out of a golden file.
    // A palette this path never actually paints with: everything below writes
    // `Doc::PlainText`, which drops every colour on the floor. That is what
    // makes the pipe safe by construction rather than by discipline — a
    // `:theme dark` in a script switches this value and still cannot put an
    // escape sequence into a file.
    Repl::Doc::Palette Plain = Repl::Doc::MonochromePalette();
    Repl::Query::Engine Queries( Session, Plain, "mono" );
    std::vector<std::string> Past;
    bool bLeaving = false;

    // True when the line was a builtin and has been answered.
    const auto Builtin = [&] ( const std::string &Statement )
    {
        const Repl::Query::Command What = Repl::Query::Parse( Statement );
        if ( What.Kind == Repl::Query::EBuiltin::None )
        {
            return false;
        }

        const Repl::Query::Result Answer = Queries.Run( What, Past );
        bLeaving                         = Answer.bExit;
        std::cout << Repl::Doc::PlainText( Answer.Body );
        std::cout.flush();

        if ( not Answer.bOk and not Answer.bExit )
        {
            Status = ExitFailure;
        }
        return true;
    };

    const auto Evaluate = [&] ( const std::string &Statement )
    {
        Past.push_back( Statement );
        if ( Builtin( Statement ) )
        {
            return;
        }

        const Repl::EvalOutcome Outcome = Session.Feed( Statement );

        std::cout << Outcome.Diagnostics;
        if ( not Outcome.Message.empty() )
        {
            std::cout << Outcome.Message << '\n';
        }

        // The value half of `=> 3 : Int32` is written by Volt code, straight
        // to the descriptor, when Echo runs it. Flushing around it is what
        // keeps the three pieces in the order they are read: this stream is
        // buffered and that one is not.
        //
        // Only when the line bound something that can render itself. A line
        // whose result has no `inspect` says nothing at all rather than
        // announcing a type with no value beside it — `puts( x )` is a side
        // effect the user came for, and a trailing `=> <StandardStream>` is
        // noise. `:type` is where a type is asked for on purpose.
        if ( not Outcome.ResultBinding.empty() )
        {
            // Echo writes the arrow and the value together, from inside the
            // JIT, straight to the descriptor. Flushing first is what keeps
            // this stream's earlier output ahead of it: this one is buffered
            // and that one is not.
            std::cout.flush();
            if ( Session.Echo( Outcome.ResultBinding ) )
            {
                std::cout.flush();
                std::cout << " : " << Outcome.ResultType << '\n';
            }
            else
            {
                std::cout << "=> #<" << Outcome.ResultType << "> : " << Outcome.ResultType << '\n';
            }
        }
        else if ( not Outcome.ResultType.empty() )
        {
            std::cout.flush();
            std::cout << "=> #<" << Outcome.ResultType << "> : " << Outcome.ResultType << '\n';
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

    // One line of lookahead, held when it was read to be inspected and turned
    // out not to continue what came before it.
    std::string Pending;
    bool bHavePending = false;

    const auto ReadLine = [&] ( std::string &Out )
    {
        if ( bHavePending )
        {
            Out          = std::move( Pending );
            bHavePending = false;
            return true;
        }
        return static_cast<bool>( std::getline( std::cin, Out ) );
    };

    while ( true )
    {
        if ( bInteractive )
        {
            std::cout << ( Statement.empty() ? Prompt : ContinuePrompt );
            std::cout.flush();
        }

        if ( not ReadLine( Line ) )
        {
            break;
        }

        Statement += Line;
        Statement += '\n';

        if ( Repl::Classify( Statement ) == Repl::ELineState::NeedsMore )
        {
            continue;
        }

        // A statement that reads as finished may still be continued by what
        // comes next: `raw_users` is a complete expression, and the `.filter`
        // on the line below belongs to it. Only a script can be asked — the
        // next line is already in the pipe — so only a script is. A terminal
        // evaluated the previous line the moment Enter was pressed, and
        // holding the prompt to find out whether a dot is coming would trade a
        // rare join for a pause on every single line.
        if ( not bInteractive )
        {
            while ( not bHavePending and std::getline( std::cin, Pending ) )
            {
                bHavePending = true;
                if ( not Repl::ContinuesPrevious( Pending ) )
                {
                    break;
                }

                Statement += Pending;
                Statement += '\n';
                bHavePending = false;
            }

            if ( Repl::Classify( Statement ) == Repl::ELineState::NeedsMore )
            {
                continue;
            }
        }

        Evaluate( Statement );
        Statement.clear();

        if ( bLeaving )
        {
            break;
        }
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
