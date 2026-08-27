#pragma once

// Terminal.hpp — the interactive front end, and the only module in this tree
// allowed to write anything.
//
// Everything below it returns values: `ReplEval` returns outcomes, `ReplQuery`
// and `ReplComplete` return Documents, `ReplSyntax` returns coloured spans,
// `ReplCore` returns a verdict about a line. This is where a value becomes
// bytes on a descriptor, and confining that to one module is what makes the
// other six testable with no terminal anywhere in sight.
//
// No alternate screen for the session itself: the transcript is ordinary
// output, so the scrollback a user already has stays theirs — the same choice
// irb makes. The one exception is the pager, which is what an alternate screen
// is actually for.

#include "ReplTui_export.hpp"

#include "Volt/ReplEval/Evaluator.hpp"

#include <cstdint>
#include <string>

namespace Volt
{

namespace Repl
{

    namespace Tui
    {

        struct SessionOptions
        {

            // Off when the terminal says it cannot, or when NO_COLOR is set.
            // A pipe never reaches this module at all.
            bool bColor = true;

            // Where the history is read from and written back to. Empty means
            // this session's history dies with it.
            std::string HistoryPath;
        };

        // Is standard input a terminal this can take over? False for a pipe, a
        // file, and a `-e` run — all of which take the plain path instead.
        [[nodiscard]] REPLTUI_EXPORT bool IsInteractiveTerminal ();

        // Where a history file belongs, by the same rules everything else
        // follows: `$VOLT_REPL_HISTORY`, then `$XDG_STATE_HOME`, then
        // `~/.local/state`. Empty when none of them resolve.
        [[nodiscard]] REPLTUI_EXPORT std::string DefaultHistoryPath ();

        // Run an interactive session until end of input or `:exit`. Returns
        // the status the process should exit with.
        [[nodiscard]] REPLTUI_EXPORT std::int32_t Run ( Evaluator &Session, const SessionOptions &Options );

    } // namespace Tui

} // namespace Repl

} // namespace Volt
