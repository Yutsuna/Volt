// Pane.cpp — framing one document, and setting two of them side by side.

#include "Volt/ReplDoc/Pane.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace Volt::Repl::Doc;

constexpr std::string_view Horizontal  = "─";
constexpr std::string_view Vertical    = "│";
constexpr std::string_view TopLeft     = "┌";
constexpr std::string_view TopRight    = "┐";
constexpr std::string_view BottomLeft  = "└";
constexpr std::string_view BottomRight = "┘";

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

} // namespace

Volt::Repl::Doc::Document Volt::Repl::Doc::FramePane ( const Pane &Panel, const Palette &Theme, const std::size_t Columns )
{
    Document Out;

    // Two glyphs of frame and two of padding; below that there is nothing left
    // to put inside, so the body is returned bare rather than in a box that
    // would be all box.
    constexpr std::size_t Furniture = 4;
    if ( Columns <= Furniture )
    {
        return WrapDocument( Panel.Body, Columns );
    }

    const std::size_t Inner = Columns - Furniture;
    const Color BorderStyle = RoleColor( Theme, EPaletteRole::PanelBorder );
    const Color TitleStyle  = RoleColor( Theme, EPaletteRole::PanelTitle );

    {
        // `+--- title ----------+`: the title rides in the top rule, the way
        // every pane in every terminal program has drawn one, so the frame
        // costs no row of its own. Four columns are furniture — the two
        // corners and the two rule glyphs before the title — and whatever the
        // title does not use is rule.
        std::string Title;
        if ( not Panel.Title.empty() )
        {
            Line Caption;
            Caption.Add( " " + Panel.Title + " " );
            const Line Cut = TruncateLine( Caption, Columns - Furniture );
            Title          = Cut.Spans.empty() ? std::string{} : Cut.Spans.front().Text;
        }

        Line Rule;
        Rule.Add( std::string( TopLeft ) + Repeat( Horizontal, 2 ), BorderStyle );
        Rule.Add( Title, TitleStyle );
        Rule.Add( Repeat( Horizontal, Columns - Furniture - DisplayWidth( Title ) ) + std::string( TopRight ), BorderStyle );
        Out.Push( std::move( Rule ) );
    }

    for ( const Line &Row : WrapDocument( Panel.Body, Inner ).Lines )
    {
        Line Framed;
        Framed.Add( std::string( Vertical ) + " ", BorderStyle );
        for ( const Span &Piece : Row.Spans )
        {
            Framed.Spans.push_back( Piece );
        }
        const std::size_t Used = LineWidth( Row );
        Framed.Add( std::string( Inner - std::min( Inner, Used ), ' ' ) );
        Framed.Add( " " + std::string( Vertical ), BorderStyle );
        Out.Push( std::move( Framed ) );
    }

    {
        Line Rule;
        Rule.Add( std::string( BottomLeft ) + Repeat( Horizontal, Columns - 2 ) + std::string( BottomRight ), BorderStyle );
        Out.Push( std::move( Rule ) );
    }

    return Out;
}

Volt::Repl::Doc::SplitLayout Volt::Repl::Doc::PlanSplit ( const std::size_t Columns )
{
    SplitLayout Plan;
    if ( Columns < MinimumSplitColumns )
    {
        Plan.LeftColumns = Columns;
        return Plan;
    }

    // Two fifths to the panel: enough for a signature and a paragraph of prose
    // without the transcript — which is what the user is actually working in —
    // losing more than it can spare.
    Plan.RightColumns = std::max<std::size_t>( 40, ( Columns * 2 ) / 5 );
    Plan.RightColumns = std::min( Plan.RightColumns, Columns - 50 );
    Plan.LeftColumns  = Columns - Plan.RightColumns;
    Plan.bSplit       = true;
    return Plan;
}

Volt::Repl::Doc::Document Volt::Repl::Doc::SideBySide ( const Document &Left, const Document &Right, const SplitLayout &Plan )
{
    Document Out;
    if ( not Plan.bSplit )
    {
        return Left;
    }

    const std::size_t Rows = std::max( Left.Lines.size(), Right.Lines.size() );
    for ( std::size_t Index = 0; Index < Rows; ++Index )
    {
        Line Row;

        const Line Half = Index < Left.Lines.size() ? TruncateLine( Left.Lines[Index], Plan.LeftColumns ) : Line{};
        for ( const Span &Piece : Half.Spans )
        {
            Row.Spans.push_back( Piece );
        }
        Row.Add( std::string( Plan.LeftColumns - std::min( Plan.LeftColumns, LineWidth( Half ) ), ' ' ) );

        const Line Other = Index < Right.Lines.size() ? TruncateLine( Right.Lines[Index], Plan.RightColumns ) : Line{};
        for ( const Span &Piece : Other.Spans )
        {
            Row.Spans.push_back( Piece );
        }
        Row.Add( std::string( Plan.RightColumns - std::min( Plan.RightColumns, LineWidth( Other ) ), ' ' ) );

        Out.Push( std::move( Row ) );
    }
    return Out;
}
