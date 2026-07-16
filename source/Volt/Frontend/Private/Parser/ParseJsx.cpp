#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Jsx.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace Volt
{

    namespace Frontend
    {

        namespace
        {

            [[nodiscard]] std::string_view Trim( std::string_view Text )
            {
                const auto IsSpace = []( char C ) { return C == ' ' || C == '\t' || C == '\r' || C == '\n'; };
                while ( !Text.empty() && IsSpace( Text.front() ) )
                {
                    Text.remove_prefix( 1 );
                }
                while ( !Text.empty() && IsSpace( Text.back() ) )
                {
                    Text.remove_suffix( 1 );
                }
                return Text;
            }

        }

        void Parser::ParseComponentFile()
        {
            ParseFile();
        }

        DeclId Parser::ParseComponent()
        {
            const std::uint32_t Begin = Here();
            Expect( TokenKind::KwComponent, "to begin a component" );

            Component Node;
            Node.Name = InternText( Expect( TokenKind::Constant, "as a component name" ) );
            if ( Accept( TokenKind::LParen ) )
            {
                ParseParameterList( Node.Params );
                Expect( TokenKind::RParen, "to close component parameters" );
            }
            SkipTerminators();
            ParseStatementBlock( Node.Body );
            Expect( TokenKind::KwEnd, "to close component" );

            return MakeDecl( std::move( Node ), RangeSince( Begin ) );
        }

        bool Parser::AtJsxStart() const
        {
            if ( PeekKind() != TokenKind::Lt )
            {
                return false;
            }
            switch ( PeekKind( 1 ) )
            {
                case TokenKind::Identifier:
                case TokenKind::Constant:
                case TokenKind::Gt: // fragment `<>`
                    return true;
                default:
                    return false;
            }
        }

        ExprId Parser::ParseJsxElement()
        {
            const std::uint32_t Begin = Here();
            Expect( TokenKind::Lt, "to begin a JSX element" );

            // Fragment: `<> ... </>`.
            if ( Accept( TokenKind::Gt ) )
            {
                JsxFragment Fragment;
                ParseJsxChildren( Fragment.Children, std::string_view{} );
                return MakeExpr( std::move( Fragment ), RangeSince( Begin ) );
            }

            JsxElement Element;
            std::string TagName;
            if ( Check( TokenKind::Identifier ) || Check( TokenKind::Constant ) )
            {
                TagName      = std::string( Spelling( Peek() ) );
                Element.Tag  = InternText( Advance() );
            }
            else
            {
                ReportHere( "expected a JSX tag name" );
            }

            // Attributes.
            while ( !AtEnd() && !Check( TokenKind::Gt ) && !Check( TokenKind::Slash ) )
            {
                SkipNewlines();
                if ( Check( TokenKind::Gt ) || Check( TokenKind::Slash ) )
                {
                    break;
                }

                // Attribute name: identifier / constant / keyword, optionally
                // namespaced (`on:click`, lexed as ident + `:click` symbol).
                std::string AttrName;
                if ( Check( TokenKind::Identifier ) || Check( TokenKind::Constant ) || IsKeyword( PeekKind() ) )
                {
                    AttrName = std::string( Spelling( Advance() ) );
                }
                else
                {
                    ReportHere( "expected a JSX attribute name" );
                    Advance();
                    continue;
                }
                while ( Check( TokenKind::SymbolLiteral ) )
                {
                    AttrName += Interner.Resolve( Advance().Lexeme );
                }

                ExprId Value;
                if ( Accept( TokenKind::Assign ) )
                {
                    if ( Accept( TokenKind::LBrace ) )
                    {
                        Value = ParseExpr( 0 );
                        Expect( TokenKind::RBrace, "to close a dynamic JSX attribute" );
                    }
                    else if ( Check( TokenKind::StringLiteral ) )
                    {
                        Value = ParseStringLiteral( Advance() );
                    }
                    else
                    {
                        ReportHere( "expected a JSX attribute value" );
                    }
                }
                else
                {
                    // Boolean attribute: `<input disabled>`.
                    BoolLiteral True;
                    True.Value = true;
                    Value      = MakeExpr( std::move( True ), RangeSince( Begin ) );
                }

                Element.AttrNames.PushBack( Interner.Intern( AttrName ) );
                Element.AttrValues.PushBack( Value );
                SkipNewlines();
            }

            if ( Accept( TokenKind::Slash ) )
            {
                Expect( TokenKind::Gt, "to close a self-closing JSX element" );
                Element.bSelfClosing = true;
                return MakeExpr( std::move( Element ), RangeSince( Begin ) );
            }

            Expect( TokenKind::Gt, "to close a JSX opening tag" );
            ParseJsxChildren( Element.Children, TagName );
            return MakeExpr( std::move( Element ), RangeSince( Begin ) );
        }

        void Parser::ParseJsxChildren( ExprList& Children, std::string_view CloseTag )
        {
            for ( ;; )
            {
                if ( AtEnd() )
                {
                    ReportHere( "unterminated JSX element" );
                    return;
                }

                // Closing tag `</tag>` or `</>`.
                if ( Check( TokenKind::Lt ) && PeekKind( 1 ) == TokenKind::Slash )
                {
                    Advance(); // '<'
                    Advance(); // '/'
                    if ( !CloseTag.empty() )
                    {
                        Expect( Check( TokenKind::Constant ) ? TokenKind::Constant : TokenKind::Identifier, "as a closing tag name" );
                    }
                    Expect( TokenKind::Gt, "to close a JSX closing tag" );
                    return;
                }

                // Nested element.
                if ( Check( TokenKind::Lt ) )
                {
                    Children.PushBack( ParseJsxElement() );
                    continue;
                }

                // Interpolation `{expr}`.
                if ( Accept( TokenKind::LBrace ) )
                {
                    Children.PushBack( ParseExpr( 0 ) );
                    Expect( TokenKind::RBrace, "to close a JSX interpolation" );
                    continue;
                }

                // Text run: everything up to the next '<' or '{', reconstructed
                // from the original source so whitespace/structure is preserved.
                const std::uint32_t TextBegin = Here();
                std::uint32_t       TextEnd   = TextBegin;
                while ( !AtEnd() && !Check( TokenKind::Lt ) && !Check( TokenKind::LBrace ) && !Check( TokenKind::KwEnd ) )
                {
                    TextEnd = Peek().Range.End;
                    Advance();
                }

                if ( TextEnd > TextBegin && TextEnd <= Source.size() )
                {
                    const std::string_view Slice = Trim( Source.substr( TextBegin, TextEnd - TextBegin ) );
                    if ( !Slice.empty() )
                    {
                        JsxText Text;
                        Text.Text = Interner.Intern( Slice );
                        Children.PushBack( MakeExpr( std::move( Text ), Core::SourceRange{ Context.FileId(), TextBegin, TextEnd } ) );
                    }
                }
                else if ( TextEnd == TextBegin )
                {
                    // No progress and not a boundary token — avoid an infinite loop.
                    return;
                }
            }
        }

    }

}
