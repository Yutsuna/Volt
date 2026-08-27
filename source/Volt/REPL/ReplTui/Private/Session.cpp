// Session.cpp — the interactive loop, and the one place a value becomes bytes.
//
// Three things happen here that cannot happen anywhere else in this tree, and
// all three are I/O:
//
//   1. The terminal goes into raw mode, and comes back out of it however this
//      returns.
//   2. The value half of `=> 42 : Int32` is *captured*. Volt writes it to
//      descriptor 1 from inside the JIT — there is no string to intercept — so
//      the descriptor is pointed at a temporary file for the length of that one
//      call, and what comes back is re-tokenized and coloured by the same
//      highlighter the input uses.
//   3. History is read from and written to a file.

#include "Volt/ReplTui/Terminal.hpp"

#include "Ansi.hpp"
#include "LineEditor.hpp"
#include "SplitPane.hpp"
#include "Terminal.hpp"

#include "Volt/ReplComplete/Completer.hpp"
#include "Volt/ReplCore/History.hpp"
#include "Volt/ReplCore/LineState.hpp"
#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"
#include "Volt/ReplQuery/QueryEngine.hpp"
#include "Volt/ReplSyntax/Highlighter.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace
{

using namespace Volt::Repl;

constexpr std::string_view PromptText   = "volt> ";
constexpr std::string_view ContinueText = "    | ";

// Run `Body` with descriptor 1 pointed at a temporary file, and hand back
// whatever it wrote there.
//
// A pipe would be the obvious choice and is the wrong one: the writer is Volt
// code running synchronously on this thread, so a value larger than the pipe
// buffer would block forever with nobody left to drain it. A file has no such
// limit and needs no second thread.
template <typename Fn> [[nodiscard]] std::string CaptureStdout ( Fn &&Body )
{
    std::FILE *Scratch = std::tmpfile();
    if ( Scratch == nullptr )
    {
        Body();
        return {};
    }

    const int Temporary = ::fileno( Scratch );
    const int Saved     = ::dup( STDOUT_FILENO );
    if ( Temporary < 0 or Saved < 0 )
    {
        std::fclose( Scratch );
        Body();
        return {};
    }

    ( void )::dup2( Temporary, STDOUT_FILENO );
    Body();
    ( void )::dup2( Saved, STDOUT_FILENO );
    ( void )::close( Saved );

    std::string Out;
    ( void )std::fseek( Scratch, 0, SEEK_SET );

    char Chunk[4096];
    while ( const std::size_t Got = std::fread( Chunk, 1, sizeof( Chunk ), Scratch ) )
    {
        Out.append( Chunk, Got );
    }
    std::fclose( Scratch );
    return Out;
}

[[nodiscard]] std::string_view Trim ( std::string_view Text )
{
    constexpr std::string_view Blank = " \t\r\n";
    const std::size_t First          = Text.find_first_not_of( Blank );
    if ( First == std::string_view::npos )
    {
        return {};
    }
    return Text.substr( First, Text.find_last_not_of( Blank ) + 1 - First );
}

void LoadHistory ( History &Past, const std::string &Path )
{
    if ( Path.empty() )
    {
        return;
    }

    std::ifstream File( Path );
    if ( not File )
    {
        return;
    }

    // One statement per line, with `\n` inside a multi-line one escaped: a
    // history file with real newlines in it could not be read back as
    // statements at all.
    std::string Line;
    while ( std::getline( File, Line ) )
    {
        std::string Statement;
        for ( std::size_t Index = 0; Index < Line.size(); ++Index )
        {
            if ( Line[Index] == '\\' and Index + 1 < Line.size() )
            {
                ++Index;
                Statement += Line[Index] == 'n' ? '\n' : Line[Index];
                continue;
            }
            Statement += Line[Index];
        }
        Past.Add( std::move( Statement ) );
    }
}

void SaveHistory ( const History &Past, const std::string &Path )
{
    if ( Path.empty() )
    {
        return;
    }

    std::error_code Ec;
    fs::create_directories( fs::path( Path ).parent_path(), Ec );

    std::ofstream File( Path, std::ios::trunc );
    if ( not File )
    {
        return;
    }

    for ( const std::string &Statement : Past.All() )
    {
        for ( const char C : Statement )
        {
            if ( C == '\\' )
            {
                File << "\\\\";
            }
            else if ( C == '\n' )
            {
                File << "\\n";
            }
            else
            {
                File << C;
            }
        }
        File << '\n';
    }
}

} // namespace

std::string Volt::Repl::Tui::DefaultHistoryPath ()
{
    if ( const char *Explicit = std::getenv( "VOLT_REPL_HISTORY" ); Explicit != nullptr and *Explicit != '\0' )
    {
        return Explicit;
    }

    if ( const char *State = std::getenv( "XDG_STATE_HOME" ); State != nullptr and *State != '\0' )
    {
        return ( fs::path( State ) / "volt" / "repl_history" ).string();
    }

    if ( const char *Home = std::getenv( "HOME" ); Home != nullptr and *Home != '\0' )
    {
        return ( fs::path( Home ) / ".local" / "state" / "volt" / "repl_history" ).string();
    }
    return {};
}

std::int32_t Volt::Repl::Tui::Run ( Evaluator &Session, const SessionOptions &Options )
{
    // Out of raw mode however this returns — a `:exit`, an end of input, or a
    // throw. A terminal left raw is a shell the user cannot type into.
    const RawMode Raw;

    const bool bColor = Options.bColor and TerminalWantsColor();
    const Doc::Palette Theme =
        bColor ? ( TerminalLooksDark() ? Doc::DefaultDarkPalette() : Doc::DefaultLightPalette() ) : Doc::MonochromePalette();

    History Past;
    LoadHistory( Past, Options.HistoryPath );

    Complete::Completer Completer( Session );
    Query::Engine Queries( Session, Theme );
    LineEditor Editor( Past, Completer, Theme, bColor );

    {
        Doc::Line Banner;
        Banner.Add( "volt repl", Doc::RoleColor( Theme, Doc::EPaletteRole::Prompt ) );
        Banner.Add( "  :help for the builtins, ^D to leave", Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
        Write( Ansi::Render( Banner, bColor ) + "\n" );
    }

    // --- What one finished statement produces ---------------------------------
    const auto Show = [&] ( const Doc::Document &Body ) { Write( Ansi::Render( Body, bColor ) ); };

    const auto Evaluate = [&] ( const std::string &Statement )
    {
        const EvalOutcome Outcome = Session.Feed( Statement );

        if ( not Outcome.Diagnostics.empty() )
        {
            // The compiler's renderer already colours what it can; what
            // reaches here is text, and text is written as text.
            Write( Outcome.Diagnostics );
        }
        if ( not Outcome.Message.empty() )
        {
            Doc::Line Row;
            Row.Add( Outcome.Message, Doc::RoleColor( Theme, Doc::EPaletteRole::Error ) );
            Write( Ansi::Render( Row, bColor ) + "\n" );
        }

        if ( Outcome.ResultBinding.empty() )
        {
            if ( not Outcome.ResultType.empty() )
            {
                // A value with no `inspect` — named, since there is nothing
                // truthful to show of it.
                Doc::Line Row;
                Row.Add( "=> ", Doc::RoleColor( Theme, Doc::EPaletteRole::ResultArrow ) );
                Row.Add( "#<" + Outcome.ResultType + ">", Doc::RoleColor( Theme, Doc::EPaletteRole::InspectBrackets ) );
                Row.Add( " : " + Outcome.ResultType, Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
                Write( Ansi::Render( Row, bColor ) + "\n" );
            }
            return;
        }

        bool bEchoed            = false;
        const std::string Value = CaptureStdout( [&] () { bEchoed = Session.Echo( Outcome.ResultBinding ); } );

        Doc::Line Row;
        Row.Add( "=> ", Doc::RoleColor( Theme, Doc::EPaletteRole::ResultArrow ) );

        if ( bEchoed )
        {
            // The rendered value, through the same lexer the input goes
            // through: a number in a result reads as a number, a string as a
            // string. `.inspect` produces Volt-shaped text by construction —
            // that is what makes re-tokenizing it meaningful rather than a
            // guess.
            for ( const Doc::Span &Piece : Syntax::HighlightVoltLine( Value, Theme ).Spans )
            {
                Row.Spans.push_back( Piece );
            }
        }
        else
        {
            Row.Add( "#<" + Outcome.ResultType + ">", Doc::RoleColor( Theme, Doc::EPaletteRole::InspectBrackets ) );
        }

        Row.Add( " : " + Outcome.ResultType, Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
        Write( Ansi::Render( Row, bColor ) + "\n" );
    };

    // --- The loop ---------------------------------------------------------------
    std::string Statement;
    while ( true )
    {
        const ReadResult Line = Editor.Read( Statement.empty() ? PromptText : ContinueText, not Statement.empty() );

        if ( Line.Status == EReadStatus::EndOfInput )
        {
            break;
        }
        if ( Line.Status == EReadStatus::Interrupted )
        {
            Statement.clear();
            continue;
        }

        // A builtin is a whole line and only ever the first one: `:type x` in
        // the middle of a `def` is text the user is typing, not a command.
        if ( Statement.empty() )
        {
            const Query::Command What = Query::Parse( Line.Text );
            if ( What.Kind != Query::EBuiltin::None )
            {
                Past.Add( Line.Text );

                // Asked afresh each time: a window can be resized between two
                // prompts, and a table sized for the old one would spill.
                Queries.SetWidth( WindowSize().Columns );

                const Query::Result Answer = Queries.Run( What, Past.All() );
                if ( Answer.bExit )
                {
                    break;
                }

                if ( Answer.Placement == Query::EPlacement::Panel and Answer.bOk )
                {
                    ShowPane( Answer.Title, Answer.Body, Theme, bColor );
                }
                else
                {
                    Show( Answer.Body );
                }
                continue;
            }
        }

        Statement += Line.Text;
        Statement += '\n';

        if ( Classify( Statement ) == ELineState::NeedsMore )
        {
            continue;
        }

        Past.Add( Trim( Statement ).empty() ? std::string{} : std::string( Trim( Statement ) ) );
        Evaluate( Statement );
        Statement.clear();
    }

    // Whatever was left when input ended is a statement the user meant to
    // finish. Evaluating it reports the syntax error rather than dropping it.
    if ( not Statement.empty() )
    {
        Evaluate( Statement );
    }

    SaveHistory( Past, Options.HistoryPath );
    return 0;
}
