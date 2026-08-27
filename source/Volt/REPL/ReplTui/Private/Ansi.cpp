// Ansi.cpp — see Ansi.hpp.

#include "Ansi.hpp"

#include <string>

std::string Volt::Repl::Tui::Ansi::Up ( const std::size_t Rows )
{
    return Rows == 0 ? std::string{} : "\x1b[" + std::to_string( Rows ) + "A";
}

std::string Volt::Repl::Tui::Ansi::Down ( const std::size_t Rows )
{
    return Rows == 0 ? std::string{} : "\x1b[" + std::to_string( Rows ) + "B";
}

std::string Volt::Repl::Tui::Ansi::Right ( const std::size_t Columns )
{
    return Columns == 0 ? std::string{} : "\x1b[" + std::to_string( Columns ) + "C";
}

std::string Volt::Repl::Tui::Ansi::Begin ( const Doc::Color Style )
{
    std::string Out;

    if ( Doc::HasAttr( Style.Attribute, Doc::EAttr::Bold ) )
    {
        Out += "\x1b[1m";
    }
    if ( Doc::HasAttr( Style.Attribute, Doc::EAttr::Faint ) )
    {
        Out += "\x1b[2m";
    }
    if ( Doc::HasAttr( Style.Attribute, Doc::EAttr::Italic ) )
    {
        Out += "\x1b[3m";
    }
    if ( Doc::HasAttr( Style.Attribute, Doc::EAttr::Underline ) )
    {
        Out += "\x1b[4m";
    }
    if ( Doc::HasAttr( Style.Attribute, Doc::EAttr::Reverse ) )
    {
        Out += "\x1b[7m";
    }

    if ( not Style.bDefault )
    {
        Out += "\x1b[38;2;" + std::to_string( Style.R ) + ";" + std::to_string( Style.G ) + ";" + std::to_string( Style.B ) + "m";
    }
    return Out;
}

std::string Volt::Repl::Tui::Ansi::Render ( const Doc::Line &Row, const bool bColor )
{
    std::string Out;
    for ( const Doc::Span &Piece : Row.Spans )
    {
        if ( not bColor )
        {
            Out += Piece.Text;
            continue;
        }

        const std::string Style = Begin( Piece.Style );
        Out += Style;
        Out += Piece.Text;
        // Reset after every styled span rather than tracking what is currently
        // on. It costs four bytes and removes an entire class of bug — a span
        // that forgot to close bleeding its colour into the rest of the line.
        Out += Style.empty() ? "" : Reset;
    }
    return Out;
}

std::string Volt::Repl::Tui::Ansi::Render ( const Doc::Document &Body, const bool bColor )
{
    std::string Out;
    for ( const Doc::Line &Row : Body.Lines )
    {
        Out += Render( Row, bColor );
        Out += '\n';
    }
    return Out;
}
