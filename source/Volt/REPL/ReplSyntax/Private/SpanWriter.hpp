#pragma once

// SpanWriter.hpp — the one thing all three highlighters do the same way.
//
// A tokenizer knows text and a role; it does not want to know that a Document
// is a list of rows and that a run containing a newline has to be split across
// them. This does that, once.

#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace Volt
{

namespace Repl
{

    namespace Syntax
    {

        class SpanWriter
        {

        public:

            SpanWriter ( const Doc::Palette &InTheme ) : Theme( InTheme )
            {
            }

            void Emit ( const std::string_view Text, const Doc::EPaletteRole Role )
            {
                Emit( Text, Doc::RoleColor( Theme, Role ) );
            }

            void Emit ( std::string_view Text, const Doc::Color Style )
            {
                while ( true )
                {
                    const std::size_t Break = Text.find( '\n' );
                    if ( Break == std::string_view::npos )
                    {
                        Current.Add( std::string( Text ), Style );
                        return;
                    }

                    Current.Add( std::string( Text.substr( 0, Break ) ), Style );
                    Out.Push( std::move( Current ) );
                    Current = Doc::Line{};
                    Text    = Text.substr( Break + 1 );
                }
            }

            // The document, with the row in progress closed off. A trailing
            // newline in the input therefore yields no empty last row, which
            // is what a caller printing row by row wants.
            [[nodiscard]] Doc::Document Finish ()
            {
                if ( not Current.Spans.empty() )
                {
                    Out.Push( std::move( Current ) );
                    Current = Doc::Line{};
                }
                return std::move( Out );
            }

            // Everything emitted so far as one row, for a caller that promised
            // there were no newlines in it.
            [[nodiscard]] Doc::Line FinishLine ()
            {
                return std::move( Current );
            }

        private:

            const Doc::Palette &Theme;
            Doc::Document Out;
            Doc::Line Current;
        };

    } // namespace Syntax

} // namespace Repl

} // namespace Volt
