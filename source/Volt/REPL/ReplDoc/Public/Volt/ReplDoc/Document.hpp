#pragma once

// Document.hpp — what every pure module in this tree returns instead of writing.
//
// A `Span` is a run of text with one colour; a `Line` is a sequence of spans;
// a `Document` is a sequence of lines. That is the whole vocabulary, and it is
// deliberately smaller than a terminal: no cursor, no absolute position, no
// escape sequence. `ReplTui` turns one of these into bytes, and a golden test
// turns the same one into a string with no terminal anywhere in sight.

#include "ReplDoc_export.hpp"
#include "Volt/ReplDoc/Palette.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Volt
{

namespace Repl
{

    namespace Doc
    {

        struct Span
        {

            std::string Text;
            Color Style;

            [[nodiscard]] bool operator==( const Span & ) const = default;
        };

        // One visual row. Held as spans rather than as pre-coloured text so a
        // consumer can measure it (`Width`), truncate it, or re-colour it
        // without parsing escape sequences back out of a string.
        struct Line
        {

            std::vector<Span> Spans;

            [[nodiscard]] bool operator==( const Line & ) const = default;

            Line &Add ( std::string Text, Color Style )
            {
                if ( not Text.empty() )
                {
                    Spans.push_back( Span{ .Text = std::move( Text ), .Style = Style } );
                }
                return *this;
            }

            Line &Add ( std::string Text )
            {
                return Add( std::move( Text ), Plain() );
            }
        };

        struct Document
        {

            std::vector<Line> Lines;

            [[nodiscard]] bool operator==( const Document & ) const = default;

            [[nodiscard]] bool Empty () const
            {
                return Lines.empty();
            }

            Document &Push ( Line Row )
            {
                Lines.push_back( std::move( Row ) );
                return *this;
            }

            Document &PushText ( std::string Text, Color Style )
            {
                Line Row;
                Row.Add( std::move( Text ), Style );
                Lines.push_back( std::move( Row ) );
                return *this;
            }

            Document &Append ( Document Other )
            {
                for ( Line &Row : Other.Lines )
                {
                    Lines.push_back( std::move( Row ) );
                }
                return *this;
            }
        };

        // How many terminal columns a string occupies.
        //
        // UTF-8 aware to the extent this REPL needs: a continuation byte
        // (`10xxxxxx`) advances no column, so accented text and box-drawing
        // characters measure as what they look like rather than as their byte
        // count. Double-width East Asian glyphs are not handled, and saying so
        // is cheaper than a half-correct table that pretends otherwise.
        [[nodiscard]] REPLDOC_EXPORT std::size_t DisplayWidth ( std::string_view Text );

        [[nodiscard]] REPLDOC_EXPORT std::size_t LineWidth ( const Line &Row );

        // The widest line in the document — what a pane needs in order to size
        // itself around its contents.
        [[nodiscard]] REPLDOC_EXPORT std::size_t DocumentWidth ( const Document &Doc );

        // The first `Columns` display columns of a row, splitting a span rather
        // than dropping it whole. Never cuts a UTF-8 sequence in half.
        [[nodiscard]] REPLDOC_EXPORT Line TruncateLine ( const Line &Row, std::size_t Columns );

        // The row, re-flowed to at most `Columns` per output line, breaking at
        // a space when there is one within the last quarter of the width and
        // mid-span otherwise. Styles ride along with the text they cover.
        [[nodiscard]] REPLDOC_EXPORT std::vector<Line> WrapLine ( const Line &Row, std::size_t Columns );

        [[nodiscard]] REPLDOC_EXPORT Document WrapDocument ( const Document &Doc, std::size_t Columns );

        // The document as plain text, one line per row, no colour. What a
        // golden test compares and what the pipe path prints.
        [[nodiscard]] REPLDOC_EXPORT std::string PlainText ( const Document &Doc );

    } // namespace Doc

} // namespace Repl

} // namespace Volt
