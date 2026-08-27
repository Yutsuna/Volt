// Document.cpp — measuring, truncating and wrapping rows of coloured text.
//
// Every function here counts *display columns*, never bytes. The difference
// matters the moment a diagnostic contains a box-drawing character or an
// accented word: a byte count would over-measure it and the frame around it
// would come out ragged.

#include "Volt/ReplDoc/Document.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

// A UTF-8 continuation byte carries no column of its own.
[[nodiscard]] bool IsContinuation ( const char Byte )
{
    return ( static_cast<unsigned char>( Byte ) & 0xC0U ) == 0x80U;
}

// The byte offset just past the `Columns`-th display column of `Text`, and how
// many columns were actually consumed (fewer, when the text ran out).
struct Cut
{

    std::size_t Bytes   = 0;
    std::size_t Columns = 0;
};

[[nodiscard]] Cut CutAt ( const std::string_view Text, const std::size_t Columns )
{
    Cut Where;
    while ( Where.Bytes < Text.size() and Where.Columns < Columns )
    {
        ++Where.Bytes;
        while ( Where.Bytes < Text.size() and IsContinuation( Text[Where.Bytes] ) )
        {
            ++Where.Bytes;
        }
        ++Where.Columns;
    }
    return Where;
}

} // namespace

std::size_t Volt::Repl::Doc::DisplayWidth ( const std::string_view Text )
{
    std::size_t Columns = 0;
    for ( const char Byte : Text )
    {
        if ( not IsContinuation( Byte ) )
        {
            ++Columns;
        }
    }
    return Columns;
}

std::size_t Volt::Repl::Doc::LineWidth ( const Line &Row )
{
    std::size_t Columns = 0;
    for ( const Span &Piece : Row.Spans )
    {
        Columns += DisplayWidth( Piece.Text );
    }
    return Columns;
}

std::size_t Volt::Repl::Doc::DocumentWidth ( const Document &Doc )
{
    std::size_t Widest = 0;
    for ( const Line &Row : Doc.Lines )
    {
        Widest = std::max( Widest, LineWidth( Row ) );
    }
    return Widest;
}

Volt::Repl::Doc::Line Volt::Repl::Doc::TruncateLine ( const Line &Row, const std::size_t Columns )
{
    Line Kept;
    std::size_t Used = 0;
    for ( const Span &Piece : Row.Spans )
    {
        if ( Used >= Columns )
        {
            break;
        }

        const Cut Where = CutAt( Piece.Text, Columns - Used );
        Kept.Add( std::string( Piece.Text.substr( 0, Where.Bytes ) ), Piece.Style );
        Used += Where.Columns;
    }
    return Kept;
}

std::vector<Volt::Repl::Doc::Line> Volt::Repl::Doc::WrapLine ( const Line &Row, const std::size_t Columns )
{
    if ( Columns == 0 or LineWidth( Row ) <= Columns )
    {
        return { Row };
    }

    // One pass over the row, emitting a break whenever the column budget runs
    // out. The break is moved back to the last space seen in the final quarter
    // of the line, which is the cheap approximation of word wrapping that
    // never leaves a one-character orphan and never scans backwards more than
    // a fraction of a row.
    std::vector<Line> Out;
    Line Current;
    std::size_t Used = 0;

    // Where the last space fell, as (span index in Current, byte offset), and
    // how many columns had been used at that point.
    std::size_t BreakSpan    = 0;
    std::size_t BreakByte    = 0;
    std::size_t BreakColumns = 0;
    bool bHaveBreak          = false;

    const auto Flush = [&] ()
    {
        Out.push_back( std::move( Current ) );
        Current      = Line{};
        Used         = 0;
        bHaveBreak   = false;
        BreakColumns = 0;
    };

    for ( const Span &Piece : Row.Spans )
    {
        std::string_view Rest = Piece.Text;
        while ( not Rest.empty() )
        {
            const Cut Where             = CutAt( Rest, Columns - Used );
            const std::string_view Head = Rest.substr( 0, Where.Bytes );

            if ( const std::size_t Space = Head.find_last_of( ' ' ); Space != std::string_view::npos )
            {
                BreakSpan    = Current.Spans.size();
                BreakByte    = Space;
                BreakColumns = Used + DisplayWidth( Head.substr( 0, Space ) );
                bHaveBreak   = true;
            }

            Current.Add( std::string( Head ), Piece.Style );
            Used += Where.Columns;
            Rest = Rest.substr( Where.Bytes );

            if ( Used < Columns )
            {
                break;
            }

            // Only rewind to a space that sits in the last quarter of the row:
            // breaking at the first word of a long line would leave most of
            // the width empty for the sake of a word boundary nobody asked for.
            if ( bHaveBreak and BreakColumns * 4 >= Columns * 3 and BreakSpan < Current.Spans.size() )
            {
                Span &Split      = Current.Spans[BreakSpan];
                std::string Tail = Split.Text.substr( BreakByte + 1 );
                Split.Text.resize( BreakByte );

                std::vector<Span> Carried;
                Carried.push_back( Span{ .Text = std::move( Tail ), .Style = Split.Style } );
                for ( std::size_t Index = BreakSpan + 1; Index < Current.Spans.size(); ++Index )
                {
                    Carried.push_back( std::move( Current.Spans[Index] ) );
                }
                Current.Spans.resize( BreakSpan + 1 );

                Flush();
                for ( Span &Piece2 : Carried )
                {
                    Used += DisplayWidth( Piece2.Text );
                    if ( not Piece2.Text.empty() )
                    {
                        Current.Spans.push_back( std::move( Piece2 ) );
                    }
                }
                continue;
            }

            Flush();
        }
    }

    if ( not Current.Spans.empty() or Out.empty() )
    {
        Out.push_back( std::move( Current ) );
    }
    return Out;
}

Volt::Repl::Doc::Document Volt::Repl::Doc::WrapDocument ( const Document &Doc, const std::size_t Columns )
{
    Document Out;
    for ( const Line &Row : Doc.Lines )
    {
        for ( Line &Wrapped : WrapLine( Row, Columns ) )
        {
            Out.Lines.push_back( std::move( Wrapped ) );
        }
    }
    return Out;
}

std::string Volt::Repl::Doc::PlainText ( const Document &Doc )
{
    std::string Out;
    for ( const Line &Row : Doc.Lines )
    {
        for ( const Span &Piece : Row.Spans )
        {
            Out += Piece.Text;
        }
        Out += '\n';
    }
    return Out;
}
