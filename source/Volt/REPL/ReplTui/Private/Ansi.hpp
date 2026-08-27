#pragma once

// Ansi.hpp — a ReplDoc colour, as the bytes a terminal understands.
//
// The one place in the project that writes an escape sequence. Everything else
// names a *role*; this is where a role that has already become a Color becomes
// `\x1b[38;2;r;g;bm`.

#include "Volt/ReplDoc/Document.hpp"
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

        namespace Ansi
        {

            inline constexpr std::string_view Reset          = "\x1b[0m";
            inline constexpr std::string_view ClearToEol     = "\x1b[K";
            inline constexpr std::string_view ClearBelow     = "\x1b[J";
            inline constexpr std::string_view ToColumnZero   = "\r";
            inline constexpr std::string_view HideCursor     = "\x1b[?25l";
            inline constexpr std::string_view ShowCursor     = "\x1b[?25h";
            inline constexpr std::string_view EnterAltScreen = "\x1b[?1049h";
            inline constexpr std::string_view LeaveAltScreen = "\x1b[?1049l";
            inline constexpr std::string_view Home           = "\x1b[H";

            [[nodiscard]] std::string Up ( std::size_t Rows );
            [[nodiscard]] std::string Down ( std::size_t Rows );
            [[nodiscard]] std::string Right ( std::size_t Columns );

            // The escape that turns `Style` on, or empty when it asks for
            // nothing at all — an uncoloured span costs no bytes.
            [[nodiscard]] std::string Begin ( Doc::Color Style );

            // One row, with colour, and `Reset` after anything that needed
            // one. `bColor` false yields the text and nothing else, which is
            // what a terminal that answers no colours gets.
            [[nodiscard]] std::string Render ( const Doc::Line &Row, bool bColor );

            // A whole document, one row per line, each ending in a newline.
            [[nodiscard]] std::string Render ( const Doc::Document &Body, bool bColor );

        } // namespace Ansi

    } // namespace Tui

} // namespace Repl

} // namespace Volt
