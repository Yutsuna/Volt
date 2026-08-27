#pragma once

// Terminal.hpp (private) — raw mode, the window's size, and one key at a time.
//
// `termios` and nothing above it: no curses, no terminfo, no dependency. What
// a REPL needs from a terminal is small and every terminal since 1979 has
// agreed on it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Volt
{

namespace Repl
{

    namespace Tui
    {

        // What one keystroke was. A byte is not enough: an arrow key arrives
        // as three, and the difference between `Escape` and the start of a
        // sequence is a timing question the reader answers, not the caller.
        enum class EKey : std::uint8_t
        {

            None = 0,
            Char, // Text holds the bytes — one codepoint, possibly multi-byte
            Enter,
            Tab,
            ShiftTab,
            Backspace,
            Delete,
            Left,
            Right,
            Up,
            Down,
            Home,
            End,
            WordLeft,
            WordRight,
            Escape,
            Interrupt,   // ^C
            EndOfInput,  // ^D on an empty line
            KillToEnd,   // ^K
            KillWord,    // ^W
            KillLine,    // ^U
            ReverseFind, // ^R
            Redraw,      // ^L
            Unknown,
        };

        struct Key
        {

            EKey Kind = EKey::None;
            std::string Text;
        };

        struct Size
        {

            std::size_t Columns = 80;
            std::size_t Rows    = 24;
        };

        // Puts the terminal into raw mode for as long as it lives, and puts it
        // back however the scope ends — including through an exception and
        // through a `:exit` that returns from the middle of the loop. A REPL
        // that left a terminal in raw mode would leave the *shell* unusable,
        // which is the one failure a user cannot recover from without knowing
        // to type `reset`.
        class RawMode
        {

        public:

            RawMode ();
            ~RawMode ();

            RawMode ( const RawMode & )           = delete;
            RawMode &operator=( const RawMode & ) = delete;

            [[nodiscard]] bool Active () const
            {
                return bActive;
            }

        private:

            bool bActive = false;
            // The caller's own settings, kept opaque so this header needs no
            // <termios.h> — every consumer of it would otherwise acquire one.
            alignas( 8 ) unsigned char Saved[64] = {};
        };

        [[nodiscard]] Size WindowSize ();

        // Blocks until a key arrives. `EndOfInput` on a closed descriptor.
        [[nodiscard]] Key ReadKey ();

        // Everything written by this module goes through here: one `write` per
        // call, unbuffered, so a prompt and the value printed under it cannot
        // arrive out of order the way two buffered streams can.
        void Write ( std::string_view Text );

        // Does the terminal look dark? `$COLORFGBG` answers when it is set —
        // `15;0` is light-on-dark — and dark is the assumption otherwise,
        // because that is what most terminals ship as.
        [[nodiscard]] bool TerminalLooksDark ();

        // Does the terminal want colour at all? False under NO_COLOR, under
        // `TERM=dumb`, and when standard output is not a terminal.
        [[nodiscard]] bool TerminalWantsColor ();

    } // namespace Tui

} // namespace Repl

} // namespace Volt
