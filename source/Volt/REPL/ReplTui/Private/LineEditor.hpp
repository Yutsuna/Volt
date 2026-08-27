#pragma once

// LineEditor.hpp — one physical line, edited in place.
//
// Deliberately one *physical* line and not a whole statement. A multi-line
// `def ... end` is accumulated by the session loop, which asks
// `ReplCore::Classify` whether what has been typed so far is finished and
// changes the prompt when it is not — so the editor never has to model a
// buffer that scrolls, and the statement's shape stays the language's question
// rather than the terminal's.
//
// What it does own: the cursor, the history walk, the reverse search, the
// completion popup and the ghost text. All of them are one line's worth of
// state, and all of them are drawn by one redraw that starts from the prompt
// and ends at the cursor.

#include "Terminal.hpp"

#include "Volt/ReplComplete/Completer.hpp"
#include "Volt/ReplCore/History.hpp"
#include "Volt/ReplDoc/Palette.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace Volt
{

namespace Repl
{

    namespace Tui
    {

        enum class EReadStatus : std::uint8_t
        {

            // `Text` holds what was typed.
            Ok = 0,
            // ^D on an empty line, or a closed descriptor. The session ends.
            EndOfInput,
            // ^C. What was typed is abandoned; the session does not end.
            Interrupted,
        };

        struct ReadResult
        {

            EReadStatus Status = EReadStatus::Ok;
            std::string Text;
        };

        class LineEditor
        {

        public:

            LineEditor ( History &InHistory, Complete::Completer &InCompleter, const Doc::Palette &InTheme, bool bInColor )
                : Past( InHistory ), Completer( InCompleter ), Theme( InTheme ), bColor( bInColor )
            {
            }

            // Read one line, drawing it as it is typed.
            //
            // `bContinuation` says the statement is unfinished, which is what
            // makes ^D on an empty line abandon that statement rather than end
            // the session.
            [[nodiscard]] ReadResult Read ( std::string_view Prompt, bool bContinuation );

        private:

            // --- Drawing -----------------------------------------------------
            void Redraw ();
            void Erase ();

            // --- Editing -----------------------------------------------------
            void Insert ( std::string_view Text );
            void Backspace ();
            void DeleteForward ();
            void KillToEnd ();
            void KillWord ();
            void KillLine ();

            [[nodiscard]] std::size_t PreviousCodepoint ( std::size_t From ) const;
            [[nodiscard]] std::size_t NextCodepoint ( std::size_t From ) const;
            [[nodiscard]] std::size_t PreviousWord ( std::size_t From ) const;
            [[nodiscard]] std::size_t NextWord ( std::size_t From ) const;

            // --- Modes -------------------------------------------------------
            void WalkHistory ( int Direction );
            [[nodiscard]] bool ReverseSearch ();
            void OpenCompletion ();
            void CloseCompletion ();
            void AcceptCompletion ();

            History &Past;
            Complete::Completer &Completer;
            const Doc::Palette &Theme;
            bool bColor = true;

            std::string Prompt;
            std::string Buffer;
            std::size_t Cursor = 0;

            // Where the history walk currently is. `Past.Size()` means "not
            // walking" — the line being typed is the user's own.
            std::size_t HistoryAt = 0;
            std::string Stashed; // the line held while walking history

            Complete::Completion Candidates;
            std::size_t Selected = 0;
            bool bCompleting     = false;

            std::string Ghost;

            // How far below the prompt row the cursor was left by the last
            // redraw, so the next one knows where to start erasing.
            std::size_t CursorRow = 0;
        };

    } // namespace Tui

} // namespace Repl

} // namespace Volt
