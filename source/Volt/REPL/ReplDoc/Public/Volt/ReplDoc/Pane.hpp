#pragma once

// Pane.hpp — a titled frame, and the two-column layout the REPL puts one in.
//
// The layout lives here rather than in `ReplTui` for the reason the whole tree
// is shaped this way: a split that decides its own column widths is a function
// from (terminal width, two documents) to one document, and a function is
// testable. What `ReplTui` is left with is writing bytes.

#include "ReplDoc_export.hpp"
#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"

#include <cstddef>
#include <string>

namespace Volt
{

namespace Repl
{

    namespace Doc
    {

        struct Pane
        {

            std::string Title;
            Document Body;
        };

        // The pane, framed and padded to exactly `Columns` display columns
        // wide. Content that does not fit is wrapped, not clipped: a signature
        // that runs off the right edge of a side panel is the one thing the
        // panel existed to show.
        [[nodiscard]] REPLDOC_EXPORT Document FramePane ( const Pane &Panel, const Palette &Theme, std::size_t Columns );

        // Below this many columns a terminal cannot hold two useful panes, so
        // the caller shows a full-width pager instead. Named here because both
        // the layout and the front end have to agree on the same number.
        inline constexpr std::size_t MinimumSplitColumns = 100;

        struct SplitLayout
        {

            std::size_t LeftColumns  = 0;
            std::size_t RightColumns = 0;
            bool bSplit              = false;
        };

        // How to divide `Columns` between a transcript and a side panel. The
        // panel takes two fifths, bounded so that neither half is uselessly
        // narrow, and `bSplit` is false below `MinimumSplitColumns` — which is
        // the front end's cue to page instead.
        [[nodiscard]] REPLDOC_EXPORT SplitLayout PlanSplit ( std::size_t Columns );

        // Two documents side by side, each already framed to its half's width,
        // padded so every output row is exactly `Columns` wide. The shorter
        // side is filled with blanks rather than the rows ending early, so a
        // redraw overwrites what was there before.
        [[nodiscard]] REPLDOC_EXPORT Document SideBySide ( const Document &Left, const Document &Right, const SplitLayout &Plan );

    } // namespace Doc

} // namespace Repl

} // namespace Volt
