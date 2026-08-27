// LineEditor.cpp — see LineEditor.hpp.

#include "LineEditor.hpp"

#include "Ansi.hpp"

#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplSyntax/Highlighter.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace
{

using namespace Volt::Repl;

[[nodiscard]] bool IsContinuation ( const char Byte )
{
    return ( static_cast<unsigned char>( Byte ) & 0xC0U ) == 0x80U;
}

[[nodiscard]] bool IsWordChar ( const char C )
{
    return ( C >= 'a' and C <= 'z' ) or ( C >= 'A' and C <= 'Z' ) or ( C >= '0' and C <= '9' ) or C == '_' or C == '?' or
           C == '!';
}

// How many rows a run of `Columns`-wide terminal holds `Width` columns in, and
// which row a given column falls on. Written once because the redraw needs
// both and getting them out of step is what makes a cursor land in the wrong
// place.
[[nodiscard]] std::size_t RowOf ( const std::size_t Column, const std::size_t Columns )
{
    return Columns == 0 ? 0 : Column / Columns;
}

} // namespace

// --- Cursor arithmetic --------------------------------------------------------

std::size_t Volt::Repl::Tui::LineEditor::PreviousCodepoint ( std::size_t From ) const
{
    if ( From == 0 )
    {
        return 0;
    }
    --From;
    while ( From > 0 and IsContinuation( Buffer[From] ) )
    {
        --From;
    }
    return From;
}

std::size_t Volt::Repl::Tui::LineEditor::NextCodepoint ( std::size_t From ) const
{
    if ( From >= Buffer.size() )
    {
        return Buffer.size();
    }
    ++From;
    while ( From < Buffer.size() and IsContinuation( Buffer[From] ) )
    {
        ++From;
    }
    return From;
}

std::size_t Volt::Repl::Tui::LineEditor::PreviousWord ( std::size_t From ) const
{
    while ( From > 0 and not IsWordChar( Buffer[From - 1] ) )
    {
        --From;
    }
    while ( From > 0 and IsWordChar( Buffer[From - 1] ) )
    {
        --From;
    }
    return From;
}

std::size_t Volt::Repl::Tui::LineEditor::NextWord ( std::size_t From ) const
{
    while ( From < Buffer.size() and not IsWordChar( Buffer[From] ) )
    {
        ++From;
    }
    while ( From < Buffer.size() and IsWordChar( Buffer[From] ) )
    {
        ++From;
    }
    return From;
}

// --- Editing --------------------------------------------------------------------

void Volt::Repl::Tui::LineEditor::Insert ( const std::string_view Text )
{
    Buffer.insert( Cursor, Text );
    Cursor += Text.size();
}

void Volt::Repl::Tui::LineEditor::Backspace ()
{
    if ( Cursor == 0 )
    {
        return;
    }
    const std::size_t Start = PreviousCodepoint( Cursor );
    Buffer.erase( Start, Cursor - Start );
    Cursor = Start;
}

void Volt::Repl::Tui::LineEditor::DeleteForward ()
{
    if ( Cursor >= Buffer.size() )
    {
        return;
    }
    Buffer.erase( Cursor, NextCodepoint( Cursor ) - Cursor );
}

void Volt::Repl::Tui::LineEditor::KillToEnd ()
{
    Buffer.erase( Cursor );
}

void Volt::Repl::Tui::LineEditor::KillWord ()
{
    const std::size_t Start = PreviousWord( Cursor );
    Buffer.erase( Start, Cursor - Start );
    Cursor = Start;
}

void Volt::Repl::Tui::LineEditor::KillLine ()
{
    Buffer.erase( 0, Cursor );
    Cursor = 0;
}

// --- Drawing ---------------------------------------------------------------------

void Volt::Repl::Tui::LineEditor::Erase ()
{
    // Back to the prompt's own row, then clear it and everything under it —
    // which is what removes the completion popup without knowing how tall it
    // was.
    std::string Out;
    Out += Ansi::Up( CursorRow );
    Out += Ansi::ToColumnZero;
    Out += Ansi::ClearBelow;
    Write( Out );
    CursorRow = 0;
}

void Volt::Repl::Tui::LineEditor::Redraw ()
{
    const Size Window          = WindowSize();
    const std::size_t Columns  = std::max<std::size_t>( 20, Window.Columns );
    const std::size_t PromptAt = Doc::DisplayWidth( Prompt );

    std::string Out;
    Out += Ansi::Up( CursorRow );
    Out += Ansi::ToColumnZero;
    Out += Ansi::ClearBelow;
    Out += Ansi::HideCursor;

    // The prompt, then the line as the compiler's own lexer sees it — with
    // the one thing a lexer cannot know filled in by the session: whether a
    // name is a type, and whether it is a function.
    const Syntax::SemanticHook Known = [this] ( const std::string_view Name, const Doc::EPaletteRole Lexical )
    { return Completer.Classify( Name, Lexical ); };

    Doc::Line Rendered;
    Rendered.Add( Prompt, Doc::RoleColor( Theme, Doc::EPaletteRole::Prompt ) );
    for ( const Doc::Span &Piece : Syntax::HighlightVoltLine( Buffer, Theme, Known ).Spans )
    {
        Rendered.Spans.push_back( Piece );
    }

    // Ghost text sits past the cursor and is never part of the buffer — it is
    // a suggestion, and pressing Right is what accepts it.
    const bool bGhost = not Ghost.empty() and Cursor == Buffer.size() and not bCompleting;
    if ( bGhost )
    {
        Rendered.Add( Ghost, Doc::RoleColor( Theme, Doc::EPaletteRole::GhostText ) );
    }

    Out += Ansi::Render( Rendered, bColor );

    // Where the terminal's cursor physically is now. A line that ends exactly
    // on a column boundary leaves it in a state no terminal agrees on — the
    // wrap is pending — so one space is written to force it, and the carriage
    // return that follows lands on the row that space created.
    const std::size_t EndColumn = PromptAt + Doc::DisplayWidth( Buffer ) + ( bGhost ? Doc::DisplayWidth( Ghost ) : 0 );
    if ( EndColumn > 0 and EndColumn % Columns == 0 )
    {
        Out += " ";
    }
    Out += Ansi::ToColumnZero;

    std::size_t PhysicalRow = RowOf( EndColumn, Columns );

    if ( bCompleting and not Candidates.Empty() )
    {
        // Below the line, so the line the user is typing never moves. Half the
        // window at most: a popup that filled the screen would push the
        // transcript out of it.
        const std::size_t Room    = Window.Rows > 6 ? ( Window.Rows / 2 ) : 3;
        const Doc::Document Popup = Complete::Completer::Render( Candidates, Theme, Selected, Room );
        for ( const Doc::Line &Row : Popup.Lines )
        {
            Out += "\n";
            Out += Ansi::Render( Doc::TruncateLine( Row, Columns ), bColor );
            Out += Ansi::ToColumnZero;
            ++PhysicalRow;
        }
    }

    const std::size_t CursorColumn = PromptAt + Doc::DisplayWidth( std::string_view( Buffer ).substr( 0, Cursor ) );
    CursorRow                      = RowOf( CursorColumn, Columns );

    Out += Ansi::Up( PhysicalRow - std::min( PhysicalRow, CursorRow ) );
    Out += Ansi::ToColumnZero;
    Out += Ansi::Right( Columns == 0 ? CursorColumn : CursorColumn % Columns );
    Out += Ansi::ShowCursor;

    Write( Out );
}

// --- History and completion -------------------------------------------------------

void Volt::Repl::Tui::LineEditor::WalkHistory ( const int Direction )
{
    if ( Past.Size() == 0 )
    {
        return;
    }

    // The line being typed is stashed on the way in and handed back on the way
    // out, so arrowing up and back down is not destructive.
    if ( HistoryAt == Past.Size() and Direction < 0 )
    {
        Stashed = Buffer;
    }

    if ( Direction < 0 )
    {
        if ( HistoryAt == 0 )
        {
            return;
        }
        --HistoryAt;
    }
    else
    {
        if ( HistoryAt >= Past.Size() )
        {
            return;
        }
        ++HistoryAt;
    }

    Buffer = HistoryAt >= Past.Size() ? Stashed : std::string( Past.At( HistoryAt ) );

    // A statement spanning several lines is remembered whole; walking back
    // into one puts its first line up and leaves the rest for the user to
    // retype, because an editor of one physical line cannot show more.
    if ( const std::size_t Break = Buffer.find( '\n' ); Break != std::string::npos )
    {
        Buffer.resize( Break );
    }
    Cursor = Buffer.size();
    Ghost.clear();
}

void Volt::Repl::Tui::LineEditor::OpenCompletion ()
{
    Candidates  = Completer.At( Buffer, Cursor );
    Selected    = 0;
    bCompleting = not Candidates.Empty();

    if ( not bCompleting )
    {
        return;
    }

    // A prefix every candidate shares is progress nobody has to choose, so it
    // goes in immediately — the popup then narrows what is left.
    const std::string Typed = Buffer.substr( Candidates.Begin, Candidates.End - Candidates.Begin );
    if ( Candidates.CommonPrefix.size() > Typed.size() )
    {
        Buffer.replace( Candidates.Begin, Candidates.End - Candidates.Begin, Candidates.CommonPrefix );
        Cursor         = Candidates.Begin + Candidates.CommonPrefix.size();
        Candidates.End = Cursor;
    }

    // One candidate is not a choice.
    if ( Candidates.Candidates.size() == 1 )
    {
        AcceptCompletion();
    }
}

void Volt::Repl::Tui::LineEditor::CloseCompletion ()
{
    bCompleting = false;
    Candidates  = Complete::Completion{};
    Selected    = 0;
}

void Volt::Repl::Tui::LineEditor::AcceptCompletion ()
{
    if ( Candidates.Empty() or Selected >= Candidates.Candidates.size() )
    {
        CloseCompletion();
        return;
    }

    const std::string &Text = Candidates.Candidates[Selected].Text;
    Buffer.replace( Candidates.Begin, Candidates.End - Candidates.Begin, Text );
    Cursor = Candidates.Begin + Text.size();
    CloseCompletion();
}

bool Volt::Repl::Tui::LineEditor::ReverseSearch ()
{
    std::string Needle;
    std::size_t From  = Past.Size() == 0 ? 0 : Past.Size() - 1;
    std::string Found = Buffer;

    while ( true )
    {
        Prompt = "(reverse-i-search)`" + Needle + "': ";
        Buffer = Found;
        Cursor = Buffer.size();
        Redraw();

        const Key Pressed = ReadKey();
        switch ( Pressed.Kind )
        {
        case EKey::Char:
            Needle += Pressed.Text;
            break;
        case EKey::Backspace:
            if ( not Needle.empty() )
            {
                Needle.pop_back();
            }
            break;
        case EKey::ReverseFind:
            // Again: keep looking, one entry further back.
            From = From == 0 ? 0 : From - 1;
            break;
        case EKey::Escape:
        case EKey::Interrupt:
            return false;
        default:
            // Anything else leaves the search with what it found — including
            // Enter, which is how a found line is accepted.
            return Pressed.Kind == EKey::Enter;
        }

        if ( const std::optional<std::size_t> Hit = Past.SearchBackwards( Needle, From ) )
        {
            From  = *Hit;
            Found = std::string( Past.At( *Hit ) );
            if ( const std::size_t Break = Found.find( '\n' ); Break != std::string::npos )
            {
                Found.resize( Break );
            }
        }
    }
}

// --- The loop -----------------------------------------------------------------------

Volt::Repl::Tui::ReadResult Volt::Repl::Tui::LineEditor::Read ( const std::string_view InPrompt, const bool bContinuation )
{
    Prompt = std::string( InPrompt );
    Buffer.clear();
    Cursor    = 0;
    CursorRow = 0;
    HistoryAt = Past.Size();
    Stashed.clear();
    Ghost.clear();
    CloseCompletion();

    Redraw();

    while ( true )
    {
        const Key Pressed = ReadKey();

        switch ( Pressed.Kind )
        {
        case EKey::EndOfInput:
            if ( not Buffer.empty() )
            {
                // ^D with something typed deletes forward, the way it does in
                // every readline there is; only an empty line ends the session.
                DeleteForward();
                break;
            }
            Write( "\n" );
            return ReadResult{ .Status = EReadStatus::EndOfInput, .Text = {} };

        case EKey::Interrupt:
            CloseCompletion();
            Write( "^C\n" );
            CursorRow = 0;
            return ReadResult{ .Status = bContinuation ? EReadStatus::Interrupted : EReadStatus::Interrupted, .Text = {} };

        case EKey::Enter:
            if ( bCompleting )
            {
                AcceptCompletion();
                break;
            }
            // Leave the finished line on screen and move past it: the
            // transcript is ordinary output and this line has just joined it.
            Ghost.clear();
            Redraw();
            Write( "\n" );
            CursorRow = 0;
            return ReadResult{ .Status = EReadStatus::Ok, .Text = Buffer };

        case EKey::Char:
            CloseCompletion();
            Insert( Pressed.Text );
            break;

        case EKey::Tab:
            if ( bCompleting )
            {
                Selected = ( Selected + 1 ) % Candidates.Candidates.size();
                break;
            }
            OpenCompletion();
            break;

        case EKey::ShiftTab:
            if ( bCompleting )
            {
                Selected = Selected == 0 ? Candidates.Candidates.size() - 1 : Selected - 1;
            }
            break;

        case EKey::Escape:
            CloseCompletion();
            break;

        case EKey::Backspace:
            CloseCompletion();
            Backspace();
            break;

        case EKey::Delete:
            CloseCompletion();
            DeleteForward();
            break;

        case EKey::Left:
            CloseCompletion();
            Cursor = PreviousCodepoint( Cursor );
            break;

        case EKey::Right:
            // At the end of the line with a suggestion showing, Right takes
            // it. Fish's rule, and the only gesture that makes ghost text
            // worth having.
            if ( Cursor == Buffer.size() and not Ghost.empty() and not bCompleting )
            {
                Insert( Ghost );
                Ghost.clear();
                break;
            }
            CloseCompletion();
            Cursor = NextCodepoint( Cursor );
            break;

        case EKey::WordLeft:
            CloseCompletion();
            Cursor = PreviousWord( Cursor );
            break;

        case EKey::WordRight:
            CloseCompletion();
            Cursor = NextWord( Cursor );
            break;

        case EKey::Up:
            if ( bCompleting )
            {
                Selected = Selected == 0 ? Candidates.Candidates.size() - 1 : Selected - 1;
                break;
            }
            WalkHistory( -1 );
            break;

        case EKey::Down:
            if ( bCompleting )
            {
                Selected = ( Selected + 1 ) % Candidates.Candidates.size();
                break;
            }
            WalkHistory( 1 );
            break;

        case EKey::Home:
            Cursor = 0;
            break;

        case EKey::End:
            Cursor = Buffer.size();
            break;

        case EKey::KillToEnd:
            KillToEnd();
            break;

        case EKey::KillWord:
            KillWord();
            break;

        case EKey::KillLine:
            KillLine();
            break;

        case EKey::ReverseFind:
        {
            const bool bAccepted = ReverseSearch();
            Prompt               = std::string( InPrompt );
            if ( bAccepted )
            {
                Redraw();
                Write( "\n" );
                CursorRow = 0;
                return ReadResult{ .Status = EReadStatus::Ok, .Text = Buffer };
            }
            break;
        }

        case EKey::Redraw:
            Write( "\x1b[2J\x1b[H" );
            CursorRow = 0;
            break;

        case EKey::None:
        case EKey::Unknown:
            break;
        }

        // The suggestion is recomputed after every edit rather than cached: it
        // depends on the whole buffer, and a stale one would offer the rest of
        // a line the buffer no longer starts.
        if ( not bCompleting )
        {
            Ghost = Complete::Completer::GhostText( Buffer, Past.All() );
        }
        Redraw();
    }
}
