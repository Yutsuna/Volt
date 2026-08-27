#pragma once

// Table.hpp — a grid of cells, and the box that draws around it.
//
// `:layout` is the reason this exists, but nothing here knows that: a Table is
// headers plus rows plus an alignment per column, and `Render` turns one into
// a Document the same way whatever asked for it. Column widths come from the
// contents, and the whole grid degrades to a narrower one rather than
// overflowing — a REPL in an eighty-column terminal is the normal case, not
// the exception.

#include "ReplDoc_export.hpp"
#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Volt
{

namespace Repl
{

    namespace Doc
    {

        enum class EAlign : std::uint8_t
        {

            Left = 0,
            Right,
            Center,
        };

        struct Cell
        {

            std::string Text;
            EPaletteRole Role = EPaletteRole::Default;
        };

        struct Table
        {

            std::vector<Cell> Headers;
            std::vector<std::vector<Cell>> Rows;
            std::vector<EAlign> Alignment; // shorter than Headers reads as Left

            [[nodiscard]] std::size_t ColumnCount () const;
        };

        // How a table draws its frame. Unicode by default because every
        // terminal this REPL runs in has had box-drawing characters for thirty
        // years; ASCII is what a golden test and a dumb pipe get.
        enum class EBorder : std::uint8_t
        {

            Unicode = 0,
            Ascii,
            None,
        };

        // The grid, as rows of coloured spans.
        //
        // `MaxColumns` of 0 means "as wide as the contents"; anything else
        // shrinks the widest columns until the whole frame fits, and elides
        // with a single `...` rather than cutting a cell off mid-word.
        [[nodiscard]] REPLDOC_EXPORT Document Render ( const Table &Grid,
                                                       const Palette &Theme,
                                                       EBorder Border         = EBorder::Unicode,
                                                       std::size_t MaxColumns = 0 );

    } // namespace Doc

} // namespace Repl

} // namespace Volt
