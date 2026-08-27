// Table.cpp — column widths from the contents, and a frame around them.

#include "Volt/ReplDoc/Table.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace Volt::Repl::Doc;

struct Glyphs
{

    std::string_view Horizontal;
    std::string_view Vertical;
    std::string_view TopLeft;
    std::string_view TopMid;
    std::string_view TopRight;
    std::string_view MidLeft;
    std::string_view MidMid;
    std::string_view MidRight;
    std::string_view BottomLeft;
    std::string_view BottomMid;
    std::string_view BottomRight;
};

[[nodiscard]] Glyphs GlyphsFor ( const EBorder Border )
{
    if ( Border == EBorder::Ascii )
    {
        return Glyphs{ "-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+" };
    }
    return Glyphs{ "─", "│", "┌", "┬", "┐", "├", "┼", "┤", "└", "┴", "┘" };
}

[[nodiscard]] std::string Repeat ( const std::string_view Unit, const std::size_t Times )
{
    std::string Out;
    Out.reserve( Unit.size() * Times );
    for ( std::size_t Index = 0; Index < Times; ++Index )
    {
        Out += Unit;
    }
    return Out;
}

// The cell text, cut to `Width` columns with a trailing ellipsis when it does
// not fit — one glyph of loss is legible, a hard cut is not.
[[nodiscard]] std::string Head ( const std::string &Text, const std::size_t Columns )
{
    Line Row;
    Row.Add( Text );
    const Line Cut = TruncateLine( Row, Columns );
    return Cut.Spans.empty() ? std::string{} : Cut.Spans.front().Text;
}

[[nodiscard]] std::string Fit ( const std::string &Text, const std::size_t Width )
{
    if ( DisplayWidth( Text ) <= Width )
    {
        return Text;
    }
    // Below four columns there is no room for both a stub and an ellipsis, so
    // the cut is hard; above it, three columns are given back to say so.
    return Width <= 3 ? Head( Text, Width ) : Head( Text, Width - 3 ) + "...";
}

[[nodiscard]] std::string Pad ( const std::string &Text, const std::size_t Width, const EAlign Align )
{
    const std::size_t Used = DisplayWidth( Text );
    if ( Used >= Width )
    {
        return Text;
    }

    const std::size_t Slack = Width - Used;
    switch ( Align )
    {
    case EAlign::Right:
        return std::string( Slack, ' ' ) + Text;
    case EAlign::Center:
        return std::string( Slack / 2, ' ' ) + Text + std::string( Slack - ( Slack / 2 ), ' ' );
    case EAlign::Left:
        break;
    }
    return Text + std::string( Slack, ' ' );
}

} // namespace

std::size_t Volt::Repl::Doc::Table::ColumnCount () const
{
    std::size_t Columns = Headers.size();
    for ( const std::vector<Cell> &Row : Rows )
    {
        Columns = std::max( Columns, Row.size() );
    }
    return Columns;
}

Volt::Repl::Doc::Document
Volt::Repl::Doc::Render ( const Table &Grid, const Palette &Theme, const EBorder Border, const std::size_t MaxColumns )
{
    Document Out;

    const std::size_t Columns = Grid.ColumnCount();
    if ( Columns == 0 )
    {
        return Out;
    }

    const auto CellAt = [&] ( const std::vector<Cell> &Row, const std::size_t Index ) -> const Cell *
    { return Index < Row.size() ? &Row[Index] : nullptr; };

    // --- Natural widths, then shrink until the frame fits --------------------
    std::vector<std::size_t> Width( Columns, 0 );
    for ( std::size_t Index = 0; Index < Columns; ++Index )
    {
        if ( const Cell *Head = CellAt( Grid.Headers, Index ); Head != nullptr )
        {
            Width[Index] = DisplayWidth( Head->Text );
        }
        for ( const std::vector<Cell> &Row : Grid.Rows )
        {
            if ( const Cell *Entry = CellAt( Row, Index ); Entry != nullptr )
            {
                Width[Index] = std::max( Width[Index], DisplayWidth( Entry->Text ) );
            }
        }
    }

    // Every column is padded by one space on each side, and every boundary
    // costs one glyph — including the two outer edges.
    const std::size_t Furniture = ( Columns * 3 ) + 1;
    if ( MaxColumns > Furniture )
    {
        const std::size_t Budget = MaxColumns - Furniture;
        std::size_t Total        = 0;
        for ( const std::size_t Each : Width )
        {
            Total += Each;
        }

        // Shave the widest column repeatedly rather than scaling everything:
        // it is the one long field (a signature, a mangled symbol) that costs
        // the width, and the narrow ones stay readable.
        while ( Total > Budget )
        {
            const auto Widest = std::max_element( Width.begin(), Width.end() );
            if ( Widest == Width.end() or *Widest <= 4 )
            {
                break;
            }
            --( *Widest );
            --Total;
        }
    }

    const Glyphs Box        = GlyphsFor( Border );
    const Color BorderStyle = RoleColor( Theme, EPaletteRole::PanelBorder );
    const bool bDrawFrame   = Border != EBorder::None;

    const auto Rule = [&] ( const std::string_view Left, const std::string_view Mid, const std::string_view Right )
    {
        Line Row;
        std::string Text( Left );
        for ( std::size_t Index = 0; Index < Columns; ++Index )
        {
            Text += Repeat( Box.Horizontal, Width[Index] + 2 );
            Text += Index + 1 == Columns ? Right : Mid;
        }
        Row.Add( std::move( Text ), BorderStyle );
        Out.Push( std::move( Row ) );
    };

    const auto Body = [&] ( const std::vector<Cell> &Cells, const EPaletteRole Fallback )
    {
        Line Row;
        if ( bDrawFrame )
        {
            Row.Add( std::string( Box.Vertical ), BorderStyle );
        }
        for ( std::size_t Index = 0; Index < Columns; ++Index )
        {
            const Cell *Entry     = CellAt( Cells, Index );
            const EAlign Align    = Index < Grid.Alignment.size() ? Grid.Alignment[Index] : EAlign::Left;
            const std::string Txt = Entry == nullptr ? std::string{} : Fit( Entry->Text, Width[Index] );
            const EPaletteRole Role =
                Entry == nullptr ? Fallback : ( Entry->Role == EPaletteRole::Default ? Fallback : Entry->Role );

            Row.Add( " " + Pad( Txt, Width[Index], Align ) + " ", RoleColor( Theme, Role ) );
            if ( bDrawFrame )
            {
                Row.Add( std::string( Box.Vertical ), BorderStyle );
            }
        }
        Out.Push( std::move( Row ) );
    };

    if ( bDrawFrame )
    {
        Rule( Box.TopLeft, Box.TopMid, Box.TopRight );
    }
    if ( not Grid.Headers.empty() )
    {
        Body( Grid.Headers, EPaletteRole::PanelTitle );
        if ( bDrawFrame )
        {
            Rule( Box.MidLeft, Box.MidMid, Box.MidRight );
        }
    }
    for ( const std::vector<Cell> &Row : Grid.Rows )
    {
        Body( Row, EPaletteRole::Default );
    }
    if ( bDrawFrame )
    {
        Rule( Box.BottomLeft, Box.BottomMid, Box.BottomRight );
    }

    return Out;
}
