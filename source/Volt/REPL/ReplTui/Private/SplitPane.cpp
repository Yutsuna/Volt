// SplitPane.cpp — see SplitPane.hpp.

#include "SplitPane.hpp"

#include "Ansi.hpp"
#include "Terminal.hpp"

#include "Volt/ReplDoc/Pane.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

void Volt::Repl::Tui::ShowPane ( const std::string_view Title,
                                 const Doc::Document &Body,
                                 const Doc::Palette &Theme,
                                 const bool bColor )
{
    const Size Window           = WindowSize();
    const Doc::SplitLayout Plan = Doc::PlanSplit( Window.Columns );

    if ( not Plan.bSplit )
    {
        Page( Title, Body, Theme, bColor );
        return;
    }

    const Doc::Pane Panel{ .Title = std::string( Title ), .Body = Body };
    const Doc::Document Framed = Doc::FramePane( Panel, Theme, Plan.RightColumns );

    // Indented into the right-hand columns, which is what puts it beside the
    // transcript rather than across it. The left columns are left empty rather
    // than filled: whatever is already there stays readable.
    std::string Out;
    for ( const Doc::Line &Row : Framed.Lines )
    {
        Out += std::string( Plan.LeftColumns, ' ' );
        Out += Ansi::Render( Row, bColor );
        Out += '\n';
    }
    Write( Out );
}

void Volt::Repl::Tui::Page ( const std::string_view Title,
                             const Doc::Document &Body,
                             const Doc::Palette &Theme,
                             const bool bColor )
{
    const Size Window = WindowSize();

    // Two rows of furniture: the title and the footer.
    const std::size_t Height   = Window.Rows > 4 ? Window.Rows - 2 : 1;
    const Doc::Document Fitted = Doc::WrapDocument( Body, Window.Columns );

    // A document that fits needs no pager at all — taking the screen away to
    // show three lines and give it back would be theatre.
    if ( Fitted.Lines.size() <= Height )
    {
        std::string Out;
        Doc::Line Heading;
        Heading.Add( std::string( Title ), Doc::RoleColor( Theme, Doc::EPaletteRole::PanelTitle ) );
        Out += Ansi::Render( Heading, bColor );
        Out += '\n';
        Out += Ansi::Render( Fitted, bColor );
        Write( Out );
        return;
    }

    Write( Ansi::EnterAltScreen );

    std::size_t Top = 0;
    while ( true )
    {
        std::string Out;
        Out += Ansi::Home;
        Out += "\x1b[2J";

        Doc::Line Heading;
        Heading.Add( std::string( Title ), Doc::RoleColor( Theme, Doc::EPaletteRole::PanelTitle ) );
        Out += Ansi::Render( Heading, bColor );
        Out += '\n';

        for ( std::size_t Row = Top; Row < std::min( Top + Height, Fitted.Lines.size() ); ++Row )
        {
            Out += Ansi::Render( Fitted.Lines[Row], bColor );
            Out += '\n';
        }

        Doc::Line Footer;
        Footer.Add( " " + std::to_string( Top + 1 ) + "-" + std::to_string( std::min( Top + Height, Fitted.Lines.size() ) ) +
                        " of " + std::to_string( Fitted.Lines.size() ) + "   j/k, space, q to close ",
                    Doc::RoleColor( Theme, Doc::EPaletteRole::Selection ) );
        Out += Ansi::Render( Footer, bColor );
        Write( Out );

        const Key Pressed = ReadKey();
        const bool bQuit  = Pressed.Kind == EKey::Escape or Pressed.Kind == EKey::Interrupt or Pressed.Kind == EKey::EndOfInput or
                           Pressed.Kind == EKey::Enter or
                           ( Pressed.Kind == EKey::Char and ( Pressed.Text == "q" or Pressed.Text == "Q" ) );
        if ( bQuit )
        {
            break;
        }

        const std::size_t Last = Fitted.Lines.size() - Height;
        if ( Pressed.Kind == EKey::Down or ( Pressed.Kind == EKey::Char and Pressed.Text == "j" ) )
        {
            Top = std::min( Top + 1, Last );
        }
        else if ( Pressed.Kind == EKey::Up or ( Pressed.Kind == EKey::Char and Pressed.Text == "k" ) )
        {
            Top = Top == 0 ? 0 : Top - 1;
        }
        else if ( Pressed.Kind == EKey::Char and Pressed.Text == " " )
        {
            Top = std::min( Top + Height, Last );
        }
        else if ( Pressed.Kind == EKey::Home )
        {
            Top = 0;
        }
        else if ( Pressed.Kind == EKey::End )
        {
            Top = Last;
        }
    }

    Write( Ansi::LeaveAltScreen );
}
