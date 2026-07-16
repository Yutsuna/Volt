#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"
#include "Volt/Frontend/Parser/Pratt.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace Volt
{

    namespace Frontend
    {

        namespace
        {

            constexpr int PrefixBindingPower = 100;

            [[nodiscard]] bool IsAssignment( TokenKind Kind )
            {
                switch ( Kind )
                {
                    case TokenKind::Assign:
                    case TokenKind::PlusEq:
                    case TokenKind::MinusEq:
                    case TokenKind::StarEq:
                    case TokenKind::SlashEq:
                    case TokenKind::PercentEq:
                    case TokenKind::PowEq:
                    case TokenKind::AmpEq:
                    case TokenKind::PipeEq:
                    case TokenKind::CaretEq:
                    case TokenKind::ShlEq:
                    case TokenKind::ShrEq:
                        return true;
                    default:
                        return false;
                }
            }

        }

        bool Parser::CanStartCommandArgument() const
        {
            switch ( PeekKind() )
            {
                case TokenKind::StringLiteral:
                case TokenKind::IntLiteral:
                case TokenKind::FloatLiteral:
                case TokenKind::CharLiteral:
                case TokenKind::SymbolLiteral:
                case TokenKind::InstanceVar:
                case TokenKind::Identifier:
                case TokenKind::Constant:
                case TokenKind::KwTrue:
                case TokenKind::KwFalse:
                case TokenKind::KwNil:
                case TokenKind::KwSelf:
                    return true;
                default:
                    return false;
            }
        }

        ExprId Parser::ParseExpr( int MinBindingPower )
        {
            ExprId Lhs = ParsePrefix();

            for ( ;; )
            {
                const TokenKind    Op = PeekKind();
                const BindingPower Bp = InfixBinding( Op );
                if ( Bp.Left == 0 || Bp.Left < MinBindingPower )
                {
                    break;
                }

                const std::uint32_t Begin = Peek().Range.Begin;
                Advance(); // operator

                if ( Op == TokenKind::Question )
                {
                    Ternary Node;
                    Node.Cond = Lhs;
                    Node.Then = ParseExpr( 0 );
                    Expect( TokenKind::Colon, "in ternary expression" );
                    Node.Else = ParseExpr( Bp.Right );
                    Lhs       = MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }
                else if ( IsAssignment( Op ) )
                {
                    Assign Node;
                    Node.Op     = Op;
                    Node.Target = Lhs;
                    Node.Value  = ParseExpr( Bp.Right );
                    Lhs         = MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }
                else
                {
                    Binary Node;
                    Node.Op  = Op;
                    Node.Lhs = Lhs;
                    Node.Rhs = ParseExpr( Bp.Right );
                    Lhs      = MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }
            }

            return Lhs;
        }

        ExprId Parser::ParsePrefix()
        {
            const std::uint32_t Begin = Here();
            const TokenKind     Kind  = PeekKind();

            switch ( Kind )
            {
                case TokenKind::Minus:
                case TokenKind::Plus:
                case TokenKind::Bang:
                case TokenKind::Tilde:
                case TokenKind::KwNot:
                case TokenKind::Amp:
                {
                    Advance();
                    Unary Node;
                    Node.Op      = Kind;
                    Node.Operand = ParseExpr( PrefixBindingPower );
                    return MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }

                case TokenKind::Star:
                {
                    Advance();
                    Deref Node;
                    Node.Operand = ParseExpr( PrefixBindingPower );
                    return MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }

                default:
                    return ParsePrimary();
            }
        }

        ExprId Parser::ParsePrimary()
        {
            const std::uint32_t Begin = Here();
            const TokenKind     Kind  = PeekKind();

            switch ( Kind )
            {
                case TokenKind::IntLiteral:
                    return ParsePostfix( MakeExpr( IntLiteral{ {}, InternText( Advance() ) }, RangeSince( Begin ) ) );

                case TokenKind::FloatLiteral:
                    return ParsePostfix( MakeExpr( FloatLiteral{ {}, InternText( Advance() ) }, RangeSince( Begin ) ) );

                case TokenKind::CharLiteral:
                    return ParsePostfix( MakeExpr( CharLiteral{ {}, InternText( Advance() ) }, RangeSince( Begin ) ) );

                case TokenKind::SymbolLiteral:
                    return ParsePostfix( MakeExpr( SymbolLiteral{ {}, InternText( Advance() ) }, RangeSince( Begin ) ) );

                case TokenKind::StringLiteral:
                {
                    const Token Tok = Advance();
                    return ParsePostfix( ParseStringLiteral( Tok ) );
                }

                case TokenKind::KwTrue:
                case TokenKind::KwFalse:
                {
                    Advance();
                    BoolLiteral Node;
                    Node.Value = ( Kind == TokenKind::KwTrue );
                    return ParsePostfix( MakeExpr( std::move( Node ), RangeSince( Begin ) ) );
                }

                case TokenKind::KwNil:
                    Advance();
                    return ParsePostfix( MakeExpr( NilLiteral{}, RangeSince( Begin ) ) );

                case TokenKind::KwSelf:
                    Advance();
                    return ParsePostfix( MakeExpr( SelfExpr{}, RangeSince( Begin ) ) );

                case TokenKind::KwSizeOf:
                {
                    Advance();
                    SizeOf Node;
                    Node.Type = ParseType();
                    return MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }

                case TokenKind::InstanceVar:
                    return ParsePostfix( MakeExpr( InstanceVar{ {}, InternText( Advance() ) }, RangeSince( Begin ) ) );

                case TokenKind::Identifier:
                {
                    const ExprId Ident = MakeExpr( Identifier{ {}, InternText( Advance() ) }, RangeSince( Begin ) );
                    // Paren-less command call: `raise "x"`, `divisible_by? 2`.
                    if ( CanStartCommandArgument() )
                    {
                        return ParseCommandCallArgs( Ident, RangeSince( Begin ) );
                    }
                    return ParsePostfix( Ident );
                }

                case TokenKind::Constant:
                    return ParsePostfix( MakeExpr( Identifier{ {}, InternText( Advance() ) }, RangeSince( Begin ) ) );

                case TokenKind::LParen:
                    return ParsePostfix( ParseParenOrGroup() );

                case TokenKind::LBracket:
                    return ParsePostfix( ParseArrayLiteral() );

                case TokenKind::LBrace:
                    return ParsePostfix( ParseHashLiteral() );

                case TokenKind::Lt:
                    if ( AtJsxStart() )
                    {
                        return ParseJsxElement();
                    }
                    [[fallthrough]];

                default:
                    ReportHere( "expected an expression" );
                    Advance();
                    return MakeExpr( NilLiteral{}, RangeSince( Begin ) );
            }
        }

        ExprId Parser::ParsePostfix( ExprId Lhs )
        {
            for ( ;; )
            {
                const std::uint32_t Begin = Here();

                if ( Accept( TokenKind::Dot ) || Accept( TokenKind::ColonColon ) )
                {
                    Member Node;
                    Node.Object = Lhs;
                    const Token& Name = Peek();
                    if ( Check( TokenKind::Identifier ) || Check( TokenKind::Constant ) )
                    {
                        Node.Name = InternText( Advance() );
                    }
                    else
                    {
                        ReportHere( "expected a member name after '.'" );
                        Node.Name = InternText( Name );
                    }
                    Lhs = MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }
                else if ( Accept( TokenKind::LParen ) )
                {
                    Call Node;
                    Node.Callee = Lhs;
                    ParseCallArguments( Node.Args, Node.ArgNames, TokenKind::RParen );
                    Expect( TokenKind::RParen, "to close call arguments" );
                    Lhs = MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }
                else if ( Accept( TokenKind::LBracket ) )
                {
                    Index Node;
                    Node.Object = Lhs;
                    SymbolList Ignored;
                    ParseCallArguments( Node.Args, Ignored, TokenKind::RBracket );
                    Expect( TokenKind::RBracket, "to close index" );
                    Lhs = MakeExpr( std::move( Node ), RangeSince( Begin ) );
                }
                else
                {
                    break;
                }
            }
            return Lhs;
        }

        void Parser::ParseCallArguments( ExprList& Args, SymbolList& ArgNames, TokenKind Close )
        {
            SkipNewlines();
            if ( Check( Close ) )
            {
                return;
            }

            do
            {
                SkipNewlines();
                if ( Check( Close ) ) // tolerate a trailing comma
                {
                    break;
                }
                Symbol Name;
                if ( Check( TokenKind::Identifier ) && PeekKind( 1 ) == TokenKind::Colon )
                {
                    Name = InternText( Advance() );
                    Advance(); // ':'
                }
                Args.PushBack( ParseExpr( 0 ) );
                ArgNames.PushBack( Name );
                SkipNewlines();
            } while ( Accept( TokenKind::Comma ) );
            SkipNewlines();
        }

        ExprId Parser::ParseCommandCallArgs( ExprId Callee, Core::SourceRange Start )
        {
            Call Node;
            Node.Callee = Callee;
            do
            {
                Symbol Name;
                if ( Check( TokenKind::Identifier ) && PeekKind( 1 ) == TokenKind::Colon )
                {
                    Name = InternText( Advance() );
                    Advance(); // ':'
                }
                Node.Args.PushBack( ParseExpr( 0 ) );
                Node.ArgNames.PushBack( Name );
            } while ( Accept( TokenKind::Comma ) );

            return MakeExpr( std::move( Node ), RangeSince( Start.Begin ) );
        }

        ExprId Parser::ParseParenOrGroup()
        {
            Expect( TokenKind::LParen, "to open a parenthesised expression" );
            SkipNewlines();
            const ExprId Inner = ParseExpr( 0 );
            SkipNewlines();
            Expect( TokenKind::RParen, "to close a parenthesised expression" );
            return Inner;
        }

        ExprId Parser::ParseArrayLiteral()
        {
            const std::uint32_t Begin = Here();
            Expect( TokenKind::LBracket, "to open an array literal" );

            ArrayLit Node;
            SkipNewlines();
            if ( !Check( TokenKind::RBracket ) )
            {
                do
                {
                    SkipNewlines();
                    if ( Check( TokenKind::RBracket ) )
                    {
                        break;
                    }
                    Node.Elements.PushBack( ParseExpr( 0 ) );
                    SkipNewlines();
                } while ( Accept( TokenKind::Comma ) );
            }
            Expect( TokenKind::RBracket, "to close an array literal" );

            if ( Accept( TokenKind::KwOf ) )
            {
                Node.ElemType = ParseType();
            }
            return MakeExpr( std::move( Node ), RangeSince( Begin ) );
        }

        ExprId Parser::ParseHashLiteral()
        {
            const std::uint32_t Begin = Here();
            Expect( TokenKind::LBrace, "to open a hash literal" );

            HashLit Node;
            SkipNewlines();
            if ( !Check( TokenKind::RBrace ) )
            {
                do
                {
                    SkipNewlines();
                    if ( Check( TokenKind::RBrace ) )
                    {
                        break;
                    }
                    Node.Keys.PushBack( ParseExpr( 0 ) );
                    Expect( TokenKind::FatArrow, "between hash key and value" );
                    Node.Values.PushBack( ParseExpr( 0 ) );
                    SkipNewlines();
                } while ( Accept( TokenKind::Comma ) );
            }
            Expect( TokenKind::RBrace, "to close a hash literal" );

            if ( Accept( TokenKind::KwOf ) )
            {
                Node.KeyType = ParseType();
                Expect( TokenKind::FatArrow, "between hash key and value types" );
                Node.ValueType = ParseType();
            }
            return MakeExpr( std::move( Node ), RangeSince( Begin ) );
        }

        ExprId Parser::ParseStringLiteral( const Token& Tok )
        {
            if ( !Tok.bHasInterpolation )
            {
                StringLiteral Node;
                Node.Value = Tok.Lexeme;
                return MakeExpr( std::move( Node ), Tok.Range );
            }

            const std::string_view Raw = Interner.Resolve( Tok.Lexeme );

            Interp        Node;
            std::size_t   LiteralStart = 0;
            std::size_t   Index        = 0;
            while ( Index < Raw.size() )
            {
                if ( Raw[Index] == '#' && Index + 1 < Raw.size() && Raw[Index + 1] == '{' )
                {
                    if ( Index > LiteralStart )
                    {
                        StringLiteral Chunk;
                        Chunk.Value = Interner.Intern( Raw.substr( LiteralStart, Index - LiteralStart ) );
                        Node.Parts.PushBack( MakeExpr( std::move( Chunk ), Tok.Range ) );
                    }

                    std::size_t Cursor = Index + 2;
                    int         Depth  = 1;
                    while ( Cursor < Raw.size() && Depth > 0 )
                    {
                        if ( Raw[Cursor] == '{' )
                        {
                            ++Depth;
                        }
                        else if ( Raw[Cursor] == '}' )
                        {
                            --Depth;
                            if ( Depth == 0 )
                            {
                                break;
                            }
                        }
                        ++Cursor;
                    }

                    const std::string_view ExprText = Raw.substr( Index + 2, Cursor - ( Index + 2 ) );
                    Node.Parts.PushBack( ParseSubExpression( ExprText, Tok.Range ) );

                    Index        = Cursor + 1;
                    LiteralStart = Index;
                }
                else
                {
                    ++Index;
                }
            }

            if ( LiteralStart < Raw.size() )
            {
                StringLiteral Chunk;
                Chunk.Value = Interner.Intern( Raw.substr( LiteralStart ) );
                Node.Parts.PushBack( MakeExpr( std::move( Chunk ), Tok.Range ) );
            }

            return MakeExpr( std::move( Node ), Tok.Range );
        }

        ExprId Parser::ParseSubExpression( std::string_view Text, Core::SourceRange Range )
        {
            Lexer              SubLexer( Context.FileId(), Text, Interner, Diagnostics );
            std::vector<Token> SubTokens = SubLexer.Tokenize();
            Parser             SubParser( std::move( SubTokens ), Context, Diagnostics );
            const ExprId       Result = SubParser.ParseExpr( 0 );
            if ( Result.IsValid() )
            {
                return Result;
            }
            return MakeExpr( NilLiteral{}, Range );
        }

    }

}
