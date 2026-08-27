#pragma once

// SplitPane.hpp — where a builtin's answer goes.
//
// The session keeps the scrollback, so there is no persistent right-hand pane
// to draw into: a two-column screen that survives across prompts needs an
// alternate screen, and giving one up was the choice made for the whole
// session (Terminal.hpp).
//
// What is left is the honest version of the same idea. In a wide terminal the
// pane is framed to the layout `ReplDoc::PlanSplit` computes and printed in the
// right-hand columns, so it sits beside the transcript and stays in the
// scrollback with it. In a narrow one there is no room for that, and the
// answer goes to a pager — which *is* what an alternate screen is for, and
// which gives the screen back untouched when it closes.

#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"

#include <string>
#include <string_view>

namespace Volt
{

namespace Repl
{

    namespace Tui
    {

        // Print a titled document beside the transcript, or page it when the
        // terminal is too narrow to have a beside.
        void ShowPane ( std::string_view Title, const Doc::Document &Body, const Doc::Palette &Theme, bool bColor );

        // A full-screen pager over one document. Returns when the reader
        // presses `q`, Escape or ^C, leaving the screen as it found it.
        void Page ( std::string_view Title, const Doc::Document &Body, const Doc::Palette &Theme, bool bColor );

    } // namespace Tui

} // namespace Repl

} // namespace Volt
