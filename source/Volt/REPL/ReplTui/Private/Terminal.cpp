// Terminal.cpp — see Terminal.hpp.

#include "Terminal.hpp"

#include "Volt/ReplTui/Terminal.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static_assert( sizeof( struct termios ) <= 64, "RawMode's opaque buffer must hold a termios" );

namespace
{

[[nodiscard]] bool EnvSaysYes ( const char *Name )
{
    const char *Value = std::getenv( Name );
    return Value != nullptr and *Value != '\0';
}

// One byte, or nothing when the descriptor is closed.
[[nodiscard]] bool ReadByte ( char &Out )
{
    while ( true )
    {
        const ssize_t Got = ::read( STDIN_FILENO, &Out, 1 );
        if ( Got == 1 )
        {
            return true;
        }
        if ( Got == 0 )
        {
            return false;
        }
        // A signal interrupted the read — a window resize, most often. Asking
        // again is right: nothing was consumed.
        if ( errno != EINTR )
        {
            return false;
        }
    }
}

// A byte, but only if one is already waiting. What tells an `Escape` keypress
// apart from the start of an arrow key: the sequence arrives in one burst, a
// lone Escape does not.
[[nodiscard]] bool PeekByte ( char &Out, const int Milliseconds )
{
    termios Current{};
    if ( ::tcgetattr( STDIN_FILENO, &Current ) != 0 )
    {
        return ReadByte( Out );
    }

    const cc_t SavedMin  = Current.c_cc[VMIN];
    const cc_t SavedTime = Current.c_cc[VTIME];

    Current.c_cc[VMIN]  = 0;
    Current.c_cc[VTIME] = static_cast<cc_t>( Milliseconds / 100 == 0 ? 1 : Milliseconds / 100 );
    ( void )::tcsetattr( STDIN_FILENO, TCSANOW, &Current );

    const ssize_t Got = ::read( STDIN_FILENO, &Out, 1 );

    Current.c_cc[VMIN]  = SavedMin;
    Current.c_cc[VTIME] = SavedTime;
    ( void )::tcsetattr( STDIN_FILENO, TCSANOW, &Current );

    return Got == 1;
}

// How many bytes the UTF-8 sequence starting with `Lead` occupies.
[[nodiscard]] std::size_t SequenceLength ( const unsigned char Lead )
{
    if ( ( Lead & 0x80U ) == 0 )
    {
        return 1;
    }
    if ( ( Lead & 0xE0U ) == 0xC0U )
    {
        return 2;
    }
    if ( ( Lead & 0xF0U ) == 0xE0U )
    {
        return 3;
    }
    if ( ( Lead & 0xF8U ) == 0xF0U )
    {
        return 4;
    }
    return 1;
}

// `\x1b[` ... — the CSI sequences a line editor cares about.
[[nodiscard]] Volt::Repl::Tui::Key ReadCsi ()
{
    using namespace Volt::Repl::Tui;

    std::string Parameters;
    char C = 0;
    while ( ReadByte( C ) )
    {
        // A CSI ends at its final byte, `@` through `~`.
        if ( C >= '@' and C <= '~' )
        {
            break;
        }
        Parameters += C;
    }

    // `\x1b[1;5C` — Ctrl held. The modifier is the parameter after the
    // semicolon, and 5 is Ctrl on every terminal that sends one.
    const bool bCtrl = Parameters.find( ";5" ) != std::string::npos;

    switch ( C )
    {
    case 'A':
        return Key{ EKey::Up, {} };
    case 'B':
        return Key{ EKey::Down, {} };
    case 'C':
        return Key{ bCtrl ? EKey::WordRight : EKey::Right, {} };
    case 'D':
        return Key{ bCtrl ? EKey::WordLeft : EKey::Left, {} };
    case 'H':
        return Key{ EKey::Home, {} };
    case 'F':
        return Key{ EKey::End, {} };
    case 'Z':
        return Key{ EKey::ShiftTab, {} };
    case '~':
        if ( Parameters.starts_with( "3" ) )
        {
            return Key{ EKey::Delete, {} };
        }
        if ( Parameters.starts_with( "1" ) or Parameters.starts_with( "7" ) )
        {
            return Key{ EKey::Home, {} };
        }
        if ( Parameters.starts_with( "4" ) or Parameters.starts_with( "8" ) )
        {
            return Key{ EKey::End, {} };
        }
        return Key{ EKey::Unknown, {} };
    default:
        break;
    }
    return Key{ EKey::Unknown, {} };
}

} // namespace

Volt::Repl::Tui::RawMode::RawMode ()
{
    termios Original{};
    if ( ::tcgetattr( STDIN_FILENO, &Original ) != 0 )
    {
        return;
    }
    std::memcpy( Saved, &Original, sizeof( Original ) );

    termios Raw = Original;

    // Character at a time, no echo, no line discipline: the editor draws what
    // was typed itself, because what it draws is coloured and what the kernel
    // would echo is not.
    Raw.c_lflag &= ~static_cast<tcflag_t>( ECHO | ICANON | IEXTEN | ISIG );
    Raw.c_iflag &= ~static_cast<tcflag_t>( IXON | ICRNL | BRKINT | INPCK | ISTRIP );
    // OPOST stays on: without it a `\n` no longer implies a carriage return,
    // and every line of ordinary transcript output would stair-step.
    Raw.c_cc[VMIN]  = 1;
    Raw.c_cc[VTIME] = 0;

    if ( ::tcsetattr( STDIN_FILENO, TCSAFLUSH, &Raw ) != 0 )
    {
        return;
    }
    bActive = true;
}

Volt::Repl::Tui::RawMode::~RawMode ()
{
    if ( not bActive )
    {
        return;
    }

    termios Original{};
    std::memcpy( &Original, Saved, sizeof( Original ) );
    ( void )::tcsetattr( STDIN_FILENO, TCSAFLUSH, &Original );
}

Volt::Repl::Tui::Size Volt::Repl::Tui::WindowSize ()
{
    Size Out;

    winsize Window{};
    if ( ::ioctl( STDOUT_FILENO, TIOCGWINSZ, &Window ) == 0 and Window.ws_col > 0 )
    {
        Out.Columns = Window.ws_col;
        Out.Rows    = Window.ws_row > 0 ? Window.ws_row : Out.Rows;
    }
    return Out;
}

Volt::Repl::Tui::Key Volt::Repl::Tui::ReadKey ()
{
    char C = 0;
    if ( not ReadByte( C ) )
    {
        return Key{ EKey::EndOfInput, {} };
    }

    switch ( C )
    {
    case '\r':
    case '\n':
        return Key{ EKey::Enter, {} };
    case '\t':
        return Key{ EKey::Tab, {} };
    case 0x7F:
    case 0x08:
        return Key{ EKey::Backspace, {} };
    case 0x01:
        return Key{ EKey::Home, {} };
    case 0x05:
        return Key{ EKey::End, {} };
    case 0x02:
        return Key{ EKey::Left, {} };
    case 0x06:
        return Key{ EKey::Right, {} };
    case 0x03:
        return Key{ EKey::Interrupt, {} };
    case 0x04:
        return Key{ EKey::EndOfInput, {} };
    case 0x0B:
        return Key{ EKey::KillToEnd, {} };
    case 0x15:
        return Key{ EKey::KillLine, {} };
    case 0x17:
        return Key{ EKey::KillWord, {} };
    case 0x12:
        return Key{ EKey::ReverseFind, {} };
    case 0x0C:
        return Key{ EKey::Redraw, {} };
    default:
        break;
    }

    if ( C == 0x1B )
    {
        char Next = 0;
        // Nothing waiting means the user pressed Escape itself.
        if ( not PeekByte( Next, 50 ) )
        {
            return Key{ EKey::Escape, {} };
        }
        if ( Next == '[' )
        {
            return ReadCsi();
        }
        if ( Next == 'O' )
        {
            // The application-cursor variants some terminals send.
            char Final = 0;
            if ( not ReadByte( Final ) )
            {
                return Key{ EKey::Escape, {} };
            }
            switch ( Final )
            {
            case 'A':
                return Key{ EKey::Up, {} };
            case 'B':
                return Key{ EKey::Down, {} };
            case 'C':
                return Key{ EKey::Right, {} };
            case 'D':
                return Key{ EKey::Left, {} };
            case 'H':
                return Key{ EKey::Home, {} };
            case 'F':
                return Key{ EKey::End, {} };
            default:
                return Key{ EKey::Unknown, {} };
            }
        }
        // Meta-b / meta-f, which is how a terminal with no Ctrl-arrow sends
        // word movement.
        if ( Next == 'b' )
        {
            return Key{ EKey::WordLeft, {} };
        }
        if ( Next == 'f' )
        {
            return Key{ EKey::WordRight, {} };
        }
        return Key{ EKey::Unknown, {} };
    }

    // A control byte nobody claimed is not text: inserting it would put an
    // unprintable character into the buffer and misalign every column count
    // downstream of it.
    if ( static_cast<unsigned char>( C ) < 0x20 )
    {
        return Key{ EKey::Unknown, {} };
    }

    Key Out{ EKey::Char, std::string( 1, C ) };
    for ( std::size_t Rest = SequenceLength( static_cast<unsigned char>( C ) ); Rest > 1; --Rest )
    {
        char Continuation = 0;
        if ( not ReadByte( Continuation ) )
        {
            break;
        }
        Out.Text += Continuation;
    }
    return Out;
}

void Volt::Repl::Tui::Write ( const std::string_view Text )
{
    std::size_t Sent = 0;
    while ( Sent < Text.size() )
    {
        const ssize_t Wrote = ::write( STDOUT_FILENO, Text.data() + Sent, Text.size() - Sent );
        if ( Wrote <= 0 )
        {
            if ( errno == EINTR )
            {
                continue;
            }
            return;
        }
        Sent += static_cast<std::size_t>( Wrote );
    }
}

bool Volt::Repl::Tui::TerminalLooksDark ()
{
    const char *Colors = std::getenv( "COLORFGBG" );
    if ( Colors == nullptr )
    {
        return true;
    }

    // `foreground;background`, sometimes with a third field. A background of
    // 0-6 or 8 is dark; anything else — 7 and 15 in particular — is light.
    const std::string_view Text( Colors );
    const std::size_t Last = Text.find_last_of( ';' );
    if ( Last == std::string_view::npos or Last + 1 >= Text.size() )
    {
        return true;
    }

    const std::string_view Background = Text.substr( Last + 1 );
    return not( Background == "7" or Background == "15" );
}

bool Volt::Repl::Tui::TerminalWantsColor ()
{
    if ( EnvSaysYes( "NO_COLOR" ) )
    {
        return false;
    }
    if ( ::isatty( STDOUT_FILENO ) != 1 )
    {
        return false;
    }

    const char *Term = std::getenv( "TERM" );
    return Term == nullptr or ( std::string_view( Term ) != "dumb" );
}

bool Volt::Repl::Tui::IsInteractiveTerminal ()
{
    return ::isatty( STDIN_FILENO ) == 1 and ::isatty( STDOUT_FILENO ) == 1;
}
