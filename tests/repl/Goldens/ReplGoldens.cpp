// ReplGoldens.cpp — the pure REPL modules, exercised with no terminal at all.
//
// Every module under source/Volt/REPL/ except ReplTui returns data rather than
// writing it, and this is what that buys: a highlighter, a table and a
// completer can each be run to a deterministic string and diffed against a
// file. There is no TTY here, no raw mode, and no escape sequence — a Document
// is printed as the roles that produced it.
//
// One section per argument, so a failure names which one moved.

#include "Volt/ReplComplete/Completer.hpp"
#include "Volt/ReplCore/History.hpp"
#include "Volt/ReplCore/LineState.hpp"
#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"
#include "Volt/ReplDoc/Pane.hpp"
#include "Volt/ReplDoc/Table.hpp"
#include "Volt/ReplEval/Evaluator.hpp"
#include "Volt/ReplQuery/QueryEngine.hpp"
#include "Volt/ReplSyntax/Highlighter.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace Volt::Repl;

// A rendered document, read back as the roles that rendered it. Only possible
// because DistinctPalette is injective — that is what it is for.
void Dump ( const Doc::Document &Body )
{
    for ( const Doc::Line &Row : Body.Lines )
    {
        std::string Text;
        for ( const Doc::Span &Piece : Row.Spans )
        {
            const Doc::EPaletteRole Role = Doc::RoleOfColor( Piece.Style );
            Text += "[";
            Text += Role == Doc::EPaletteRole::Count ? "-" : std::string( Doc::RoleName( Role ) );
            Text += " ";
            Text += Piece.Text;
            Text += "]";
        }
        std::cout << Text << '\n';
    }
}

void DumpPlain ( const Doc::Document &Body )
{
    std::cout << Doc::PlainText( Body );
}

void Section ( const std::string_view Name )
{
    std::cout << "--- " << Name << " ---\n";
}

// --- ReplSyntax ---------------------------------------------------------------

void SyntaxSection ()
{
    const Doc::Palette Theme = Doc::DistinctPalette();

    Section( "volt: a complete line" );
    Dump( Syntax::HighlightVolt( "def twice( n : Int32 ) -> Int32\n  n * 2 # doubled\nend\n", Theme ) );

    Section( "volt: literals" );
    Dump( Syntax::HighlightVolt( "x = [ 1, 2.5, \"hi\", :sym, true, nil ]\n", Theme ) );

    // The point of the whole design: a line that does not parse still colours.
    Section( "volt: unfinished" );
    Dump( Syntax::HighlightVolt( "def broken( n :\n", Theme ) );

    Section( "volt: unterminated string" );
    Dump( Syntax::HighlightVolt( "puts( \"never closed\n", Theme ) );

    Section( "volt: doc block being typed" );
    Dump( Syntax::HighlightVolt( "#{ @brief half a block\n", Theme ) );

    Section( "ir" );
    Dump( Syntax::HighlightIr( "define void @_V_init_3() {\nentry:\n  %0 = add i32 %n, 1 ; a comment\n  ret void\n}\n", Theme ) );

    Section( "asm" );
    Dump( Syntax::HighlightAsm( "0x00007f00  leal (%rdi,%rdi), %eax\n  movl $42, -4(%rsp)\n  retq\n", Theme ) );
}

// --- ReplDoc ------------------------------------------------------------------

[[nodiscard]] Doc::Table SampleTable ()
{
    Doc::Table Grid;
    Grid.Headers   = { Doc::Cell{ "Field" }, Doc::Cell{ "Type" }, Doc::Cell{ "Offset" } };
    Grid.Alignment = { Doc::EAlign::Left, Doc::EAlign::Left, Doc::EAlign::Right };
    Grid.Rows      = {
        { Doc::Cell{ "data" }, Doc::Cell{ "ptr" }, Doc::Cell{ "0" } },
        { Doc::Cell{ "size" }, Doc::Cell{ "i64" }, Doc::Cell{ "8" } },
        { Doc::Cell{ "owns_a_very_long_buffer_name" }, Doc::Cell{ "i1" }, Doc::Cell{ "16" } },
    };
    return Grid;
}

void DocSection ()
{
    const Doc::Palette Theme = Doc::DistinctPalette();

    Section( "table: as wide as it wants" );
    DumpPlain( Doc::Render( SampleTable(), Theme ) );

    Section( "table: squeezed into 34 columns" );
    DumpPlain( Doc::Render( SampleTable(), Theme, Doc::EBorder::Unicode, 34 ) );

    Section( "table: ascii" );
    DumpPlain( Doc::Render( SampleTable(), Theme, Doc::EBorder::Ascii ) );

    Section( "wrap: 20 columns" );
    {
        Doc::Line Row;
        Row.Add( "the quick brown fox jumps over the lazy dog and keeps going" );
        for ( const Doc::Line &Wrapped : Doc::WrapLine( Row, 20 ) )
        {
            std::cout << "|" << Doc::PlainText( Doc::Document{ { Wrapped } } );
        }
    }

    Section( "pane: 40 columns" );
    {
        Doc::Pane Panel;
        Panel.Title = "doc: Array.map";
        Panel.Body.PushText( "Returns a new array with the results of running the block once for every element.", Doc::Plain() );
        DumpPlain( Doc::FramePane( Panel, Theme, 40 ) );
    }

    Section( "split: 120, 99 and 80 columns" );
    for ( const std::size_t Columns : { std::size_t{ 120 }, std::size_t{ 99 }, std::size_t{ 80 } } )
    {
        const Doc::SplitLayout Plan = Doc::PlanSplit( Columns );
        std::cout << Columns << " -> split=" << ( Plan.bSplit ? "yes" : "no" ) << " left=" << Plan.LeftColumns
                  << " right=" << Plan.RightColumns << '\n';
    }

    Section( "side by side" );
    {
        Doc::Document Left;
        Left.PushText( "volt> 1 + 1", Doc::Plain() );
        Left.PushText( "=> 2 : Int32", Doc::Plain() );

        Doc::Document Right;
        Right.PushText( "| panel |", Doc::Plain() );

        const Doc::SplitLayout Plan = Doc::PlanSplit( 100 );
        for ( const Doc::Line &Row : Doc::SideBySide( Left, Right, Plan ).Lines )
        {
            std::cout << "|" << Doc::PlainText( Doc::Document{ { Row } } );
        }
    }
}

// --- ReplCore -----------------------------------------------------------------

void CoreSection ()
{
    Section( "classify" );
    for ( const std::string_view Input :
          { "1 + 1\n", "def f\n", "def f\n  1\nend\n", "[ 1, 2,\n", "x = 1 +\n", "\"unterminated\n", "x = 1 if y\n" } )
    {
        std::string Shown( Input );
        for ( char &C : Shown )
        {
            C = C == '\n' ? '/' : C;
        }
        std::cout << ( Classify( Input ) == ELineState::NeedsMore ? "more " : "done " ) << Shown << '\n';
    }

    Section( "history" );
    {
        History Past;
        Past.Add( "x = 1" );
        Past.Add( "x = 1" ); // an immediate repeat is not remembered twice
        Past.Add( "   " );   // nor is a blank line
        Past.Add( "puts( x )" );
        Past.Add( "x = 2" );

        std::cout << "size " << Past.Size() << '\n';
        for ( const std::string &Line : Past.All() )
        {
            std::cout << "  " << Line << '\n';
        }

        const auto Found = Past.SearchBackwards( "x =", Past.Size() - 1 );
        std::cout << "search 'x =' -> " << ( Found ? Past.At( *Found ) : "(none)" ) << '\n';

        const auto Missing = Past.SearchBackwards( "nope", Past.Size() - 1 );
        std::cout << "search 'nope' -> " << ( Missing ? "found" : "(none)" ) << '\n';
    }

    Section( "ghost text" );
    {
        const std::vector<std::string> Past = { "puts( 1 )", "put_away", "puts( 2 )" };
        std::cout << "'put'  -> '" << Complete::Completer::GhostText( "put", Past ) << "'\n";
        std::cout << "'puts' -> '" << Complete::Completer::GhostText( "puts", Past ) << "'\n";
        std::cout << "'zzz'  -> '" << Complete::Completer::GhostText( "zzz", Past ) << "'\n";
    }
}

// --- ReplQuery ------------------------------------------------------------------

void ParseSection ()
{
    Section( "parse" );
    for ( const std::string_view Line : { ":type x", ":layout Int32", ":bench 50 f()", ":help", ":pending", "x = 1", ":" } )
    {
        const Query::Command What = Query::Parse( Line );
        std::cout << "'" << Line << "' -> "
                  << ( What.Kind == Query::EBuiltin::None ? std::string( "(not a builtin)" )
                                                          : ":" + What.Name + " | " + What.Argument )
                  << '\n';
    }

    Section( "builtin table" );
    for ( const Query::Builtin &Each : Query::Builtins() )
    {
        std::cout << ":" << Each.Name << ( Each.bPanel ? " [panel]" : "" ) << '\n';
    }
}

// --- ReplComplete, over a real session -------------------------------------------

void CompleteSection ()
{
    Section( "builtins after ':'" );
    {
        // No session needed: the builtin table is static.
        Evaluator Unused;
        Complete::Completer Completer( Unused );

        for ( const std::string &Line : { std::string( ":" ), std::string( ":d" ) } )
        {
            const Complete::Completion What = Completer.At( Line, Line.size() );
            std::cout << "'" << Line << "' prefix '" << What.CommonPrefix << "'\n";
            for ( const Complete::Candidate &Each : What.Candidates )
            {
                std::cout << "  " << Each.Text << '\n';
            }
        }
    }

    Evaluator Session;
    std::string Error;
    if ( not Session.Start( EvaluatorOptions{}, Error ) )
    {
        std::cout << "session did not start: " << Error << '\n';
        return;
    }

    ( void )Session.Feed( "greeting = \"hello world\"\n" );

    Complete::Completer Completer( Session );

    Section( "a method's signature: 'greeting.sta'" );
    {
        const std::string Line          = "greeting.sta";
        const Complete::Completion What = Completer.At( Line, Line.size() );
        for ( const Complete::Candidate &Each : What.Candidates )
        {
            std::cout << "  " << Each.Text << "  " << Each.Detail << '\n';
        }
    }

    Section( "members after 'greeting.si'" );
    {
        const std::string Line          = "greeting.si";
        const Complete::Completion What = Completer.At( Line, Line.size() );
        for ( const Complete::Candidate &Each : What.Candidates )
        {
            std::cout << "  " << Each.Text << "  " << Each.Detail << '\n';
        }
    }

    Section( "bare identifier 'gree'" );
    {
        const std::string Line          = "gree";
        const Complete::Completion What = Completer.At( Line, Line.size() );
        for ( const Complete::Candidate &Each : What.Candidates )
        {
            std::cout << "  " << Each.Text << "  " << Each.Detail << '\n';
        }
    }

    Section( "no receiver" );
    {
        const std::string Line          = "+.";
        const Complete::Completion What = Completer.At( Line, Line.size() );
        std::cout << "  candidates " << What.Candidates.size() << '\n';
    }
}

} // namespace

int main ( int argc, char **argv )
{
    const std::string_view Which = argc > 1 ? std::string_view( argv[1] ) : std::string_view( "syntax" );

    if ( Which == "syntax" )
    {
        SyntaxSection();
    }
    else if ( Which == "doc" )
    {
        DocSection();
    }
    else if ( Which == "core" )
    {
        CoreSection();
    }
    else if ( Which == "parse" )
    {
        ParseSection();
    }
    else if ( Which == "complete" )
    {
        CompleteSection();
    }
    else
    {
        std::cerr << "unknown section '" << Which << "'\n";
        return 2;
    }
    return 0;
}
