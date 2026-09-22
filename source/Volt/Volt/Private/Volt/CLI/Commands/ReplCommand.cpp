#include "Volt/CLI/Commands/ReplCommand.hpp"
#include "Volt/CLI/CommandInputs.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/StdlibCache.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Driver/Driver.hpp"
#include "Volt/Driver/WellKnown.hpp"
#include "Volt/ReplCore/LineState.hpp"
#include "Volt/ReplEval/Evaluator.hpp"
#include "Volt/ReplQuery/QueryEngine.hpp"
#include "Volt/ReplTui/Terminal.hpp"

#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{

constexpr std::string_view Prompt         = "volt> ";
constexpr std::string_view ContinuePrompt = "    | ";

[[nodiscard]] bool StandardInputIsATerminal ()
{
    return isatty( STDIN_FILENO ) == 1;
}

/// Read an entire file into a string. Returns empty on failure (error
/// logged).
[[nodiscard]] std::string ReadFileContents ( const fs::path &Path )
{
    std::ifstream Stream( Path, std::ios::binary );
    if ( not Stream )
    {
        Volt::Core::FLogger::Error( "Cannot read '" + Path.string() + "'", "repl" );
        return {};
    }

    std::ostringstream Buffer;
    Buffer << Stream.rdbuf();
    return Buffer.str();
}

/// Split source text into complete Volt statements, respecting block
/// nesting, string literals and continuation lines. Each returned string
/// is one statement terminated by a newline.
[[nodiscard]] std::vector<std::string> SplitIntoStatements ( const std::string &Source )
{
    std::vector<std::string> Statements;
    std::string Statement;
    std::string Line;
    std::istringstream Stream( Source );

    while ( std::getline( Stream, Line ) )
    {
        Statement += Line;
        Statement += '\n';

        if ( Volt::Repl::Classify( Statement ) == Volt::Repl::ELineState::NeedsMore )
        {
            continue;
        }

        // Consume continuation lines (leading `.` / `|>` / `,` etc.).
        std::string Pending;
        bool bHavePending = false;
        while ( std::getline( Stream, Pending ) )
        {
            bHavePending = true;
            if ( not Volt::Repl::ContinuesPrevious( Pending ) )
            {
                break;
            }

            Statement += Pending;
            Statement += '\n';
            bHavePending = false;
        }

        if ( Volt::Repl::Classify( Statement ) == Volt::Repl::ELineState::NeedsMore )
        {
            continue;
        }

        Statements.push_back( std::move( Statement ) );
        Statement.clear();

        // Put back the non-continuation line we consumed as lookahead.
        if ( bHavePending )
        {
            // We need to re-process this line in the outer loop. The
            // simplest correct approach: push it back by prepending it to
            // a new istringstream. Since we already consumed part of the
            // original stream, we reconstruct the remainder.
            std::ostringstream Remainder;
            Remainder << Pending << '\n';
            std::string RestLine;
            while ( std::getline( Stream, RestLine ) )
            {
                Remainder << RestLine << '\n';
            }

            // Replace the stream with the remainder.
            Stream = std::istringstream( Remainder.str() );
            bHavePending = false;
        }
    }

    // Whatever was left is a trailing statement the user meant to finish.
    if ( not Statement.empty() )
    {
        Statements.push_back( std::move( Statement ) );
    }

    return Statements;
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
    return "volt repl [options] [input_file]";
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

    for ( FOption &Option : GetInputOptions( InputFlags, "File or circuit to load first" ) )
    {
        Options.push_back( std::move( Option ) );
    }

    for ( FOption &Option : StdlibCacheOptions( StdlibFlags ) )
    {
        Options.push_back( std::move( Option ) );
    }
    return Options;
}

// ---------------------------------------------------------------------------
// File / circuit loading
// ---------------------------------------------------------------------------

std::int32_t Volt::CLI::FReplCommand::LoadFile ( Repl::Evaluator &Session, const fs::path &FilePath )
{
    const std::string Source = ReadFileContents( FilePath );
    if ( Source.empty() )
    {
        return ExitFailure;
    }

    Core::FLogger::Info( "Loading " + FilePath.string(), "repl" );

    const std::vector<std::string> Statements = SplitIntoStatements( Source );
    std::int32_t Status = ExitSuccess;

    for ( const std::string &Stmt : Statements )
    {
        const Repl::EvalOutcome Outcome = Session.Feed( Stmt );

        if ( not Outcome.Diagnostics.empty() )
        {
            std::cout << Outcome.Diagnostics;
        }

        if ( Outcome.Status != Repl::EEvalStatus::Ok )
        {
            Status = ExitFailure;
            if ( Outcome.Status == Repl::EEvalStatus::DidNotRun and not Outcome.Message.empty() )
            {
                Core::FLogger::Error( Outcome.Message, "repl" );
            }
        }
    }

    std::cout.flush();
    return Status;
}

std::int32_t Volt::CLI::FReplCommand::LoadCircuit ( Repl::Evaluator &Session, const fs::path &ManifestPath )
{
    // Compile the circuit through the Driver so we get full sema, then feed
    // each unit's source text into the REPL session in file order.
    const Driver::FCacheOptions CacheOpts = ToDriverCacheOptions( StdlibFlags );

    Driver::Driver TheDriver;
    Core::FLogger::Info( "Loading circuit from " + ManifestPath.parent_path().string(), "repl" );
    Core::FLogger::Progress( "Compiling circuit...", "repl" );

    const Driver::CompileResult Compiled = TheDriver.CompileCircuit( ManifestPath.string(), CacheOpts );

    Core::FLogger::Progress( TheDriver.HasErrors() ? "Circuit compilation failed" : "Circuit compiled", "repl",
                             /*bFinished=*/true );

    if ( TheDriver.DiagnosticCount() > 0 )
    {
        Core::FLogger::Flush();
        TheDriver.RenderDiagnostics( std::cerr );
    }

    if ( TheDriver.HasErrors() )
    {
        Core::FLogger::Error(
            std::to_string( Compiled.Errors ) + " error(s) across " + std::to_string( Compiled.Files ) + " file(s)", "repl" );
        return ExitFailure;
    }

    // Feed each compiled unit's source text into the REPL. The REPL's own
    // Driver will re-compile and JIT-evaluate them, which is the only way
    // to make their definitions live in the session.
    std::int32_t Status = ExitSuccess;
    for ( std::size_t I = 0; I < TheDriver.UnitCount(); ++I )
    {
        const Driver::CompileUnit &Unit = TheDriver.UnitAt( I );
        const std::string &Path         = Unit.Path;

        // Skip the stdlib units — the REPL session already has its own
        // stdlib loaded at Start().
        if ( I < TheDriver.StdlibUnitCount() )
        {
            continue;
        }

        const std::string Source = ReadFileContents( Path );
        if ( Source.empty() )
        {
            Status = ExitFailure;
            continue;
        }

        Core::FLogger::Info( "Loading " + Path, "repl" );

        const std::vector<std::string> Statements = SplitIntoStatements( Source );
        for ( const std::string &Stmt : Statements )
        {
            const Repl::EvalOutcome Outcome = Session.Feed( Stmt );

            if ( not Outcome.Diagnostics.empty() )
            {
                std::cout << Outcome.Diagnostics;
            }

            if ( Outcome.Status != Repl::EEvalStatus::Ok )
            {
                Status = ExitFailure;
                if ( Outcome.Status == Repl::EEvalStatus::DidNotRun and not Outcome.Message.empty() )
                {
                    Core::FLogger::Error( Outcome.Message, "repl" );
                }
            }
        }
    }

    std::cout.flush();
    return Status;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

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

    // Resolve the input file: explicit -i/--input wins, then positional, then
    // nothing (bare interactive session).
    std::string InputPath;
    if ( not InputFlags.ExplicitInput.empty() )
    {
        InputPath = InputFlags.ExplicitInput;
    }
    else if ( not Parsed->Positionals.empty() )
    {
        InputPath = std::string( Parsed->Positionals.front() );
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

    // -----------------------------------------------------------------------
    // Load file / circuit before the interactive loop
    // -----------------------------------------------------------------------
    std::int32_t LoadStatus = ExitSuccess;
    if ( not InputPath.empty() )
    {
        std::error_code Ec;
        const fs::path Input( InputPath );

        if ( not fs::exists( Input, Ec ) )
        {
            Core::FLogger::Error( "Cannot read '" + InputPath + "': no such file or directory", "repl" );
            return ExitFailure;
        }

        // Directory with a Project.vl → treat as a circuit.
        if ( fs::is_directory( Input, Ec ) )
        {
            const fs::path Manifest = Input / Driver::WellKnown::ManifestName;
            if ( fs::is_regular_file( Manifest, Ec ) )
            {
                LoadStatus = LoadCircuit( Session, Manifest );
            }
            else
            {
                Core::FLogger::Error( "Directory '" + InputPath + "' has no " +
                                       std::string( Driver::WellKnown::ManifestName ), "repl" );
                return ExitFailure;
            }
        }
        else
        {
            LoadStatus = LoadFile( Session, Input );
        }

        if ( LoadStatus != ExitSuccess )
        {
            // Errors were already printed. Fall through to the interactive
            // prompt so the user can inspect or retry — do not abort.
        }
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
    std::int32_t Status = LoadStatus;

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
