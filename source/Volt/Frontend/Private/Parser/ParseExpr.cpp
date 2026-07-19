#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"
#include "Volt/Frontend/Parser/Pratt.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace Volt
{

namespace Frontend
{

    namespace
    {

        // Only these expression kinds denote something a trailing `do ... end`
        // / `{ ... }` block can sensibly attach to (a callee); literals, hash
        // literals, and the like can never be "called" with a block.
        [[nodiscard]] bool IsBlockAttachable ( ExprKind Kind )
        {
            switch ( Kind )
            {
            case ExprKind::Identifier:
            case ExprKind::Member:
            case ExprKind::Call:
            case ExprKind::Index:
                return true;
            default:
                return false;
            }
        }

    } // namespace

    bool Parser::CanStartCommandArgument () const
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

    ExprId Parser::ParseExpr ( int MinBindingPower )
    {
        ExprId Lhs = ParsePrefix();

        for ( ;; )
        {
            const TokenKind Op    = PeekKind();
            const BindingPower Bp = InfixBinding( Op );
            if ( Bp.Left == 0 or Bp.Left < MinBindingPower )
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

    ExprId Parser::ParsePrefix ()
    {
        const std::uint32_t Begin = Here();
        const TokenKind Kind      = PeekKind();

        // Every VOLT_PREFIX row of Pratt.inl lowers to a Unary node here; a
        // new prefix operator is one manifest line, never a new case.
        if ( IsPrefixOperator( Kind ) )
        {
            Advance();
            Unary Node;
            Node.Op      = Kind;
            Node.Operand = ParseExpr( PrefixBindingPower );
            return MakeExpr( std::move( Node ), RangeSince( Begin ) );
        }

        // `*p` builds a Deref node, not a Unary — structural, so not in the manifest.
        if ( Kind == TokenKind::Star )
        {
            Advance();
            Deref Node;
            Node.Operand = ParseExpr( PrefixBindingPower );
            return MakeExpr( std::move( Node ), RangeSince( Begin ) );
        }

        return ParsePrimary();
    }

    ExprId Parser::ParsePrimary ()
    {
        const std::uint32_t Begin = Here();
        const TokenKind Kind      = PeekKind();

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

    ExprId Parser::ParsePostfix ( ExprId Lhs )
    {
        for ( ;; )
        {
            const std::uint32_t Begin = Here();

            if ( Accept( TokenKind::Dot ) or Accept( TokenKind::ColonColon ) )
            {
                Member Node;
                Node.Object       = Lhs;
                const Token &Name = Peek();
                if ( Check( TokenKind::Identifier ) or Check( TokenKind::Constant ) )
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
            else if ( ( ( Check( TokenKind::KwDo ) and !bNoDoBlock ) or Check( TokenKind::LBrace ) ) and IsBlockAttachable( KindOf( Context.Expr( Lhs ) ) ) )
            {
                // Trailing `do |x| ... end` / `{ |x| ... }` block — same
                // construct, two spellings.
                Lhs = AttachTrailingBlock( Lhs, ParseDoBlock(), Begin );
            }
            else
            {
                break;
            }
        }
        return Lhs;
    }

    void Parser::ParseCallArguments ( ExprList &Args, SymbolList &ArgNames, TokenKind Close )
    {
        SkipNewlines();
        if ( Check( Close ) )
        {
            return;
        }

        // Delimited by parens/brackets: a fresh context where `do` may
        // attach again (`foo(x.each do ... end)`).
        const bool Saved = bNoDoBlock;
        bNoDoBlock       = false;
        do
        {
            SkipNewlines();
            if ( Check( Close ) ) // tolerate a trailing comma
            {
                break;
            }
            Symbol Name;
            if ( Check( TokenKind::Identifier ) and PeekKind( 1 ) == TokenKind::Colon )
            {
                Name = InternText( Advance() );
                Advance(); // ':'
            }
            Args.PushBack( ParseExpr( 0 ) );
            ArgNames.PushBack( Name );
            SkipNewlines();
        } while ( Accept( TokenKind::Comma ) );
        bNoDoBlock = Saved;
        SkipNewlines();
    }

    ExprId Parser::AttachTrailingBlock ( ExprId Lhs, ExprId BlockId, std::uint32_t Begin )
    {
        // Reach into the arena and mutate the existing Call in place when
        // possible (`each do`, `v.each do`) rather than nesting a wrapper
        // Call around it — this is the one deliberate exception to the
        // value-AST convention that nodes are replaced via Context.Add, not
        // edited through a live reference. It is safe because no further
        // Context.Add call happens between the lookup and the assignment.
        if ( Call *AsCall = std::get_if<Call>( &Context.Expr( Lhs ) ); AsCall != nullptr and !AsCall->BlockArg.IsValid() )
        {
            AsCall->BlockArg = BlockId;
            return Lhs;
        }

        // A bare callee (e.g. a block on a local/identifier) has no Call to
        // mutate — wrap it in one.
        Call Node;
        Node.Callee   = Lhs;
        Node.BlockArg = BlockId;
        return MakeExpr( std::move( Node ), RangeSince( Begin ) );
    }

    ExprId Parser::ParseDoBlock ()
    {
        const std::uint32_t Begin = Here();

        // `do ... end` and `{ ... }` are the same block literal, just
        // spelled with different delimiters (Ruby/Crystal).
        const bool bBrace = Check( TokenKind::LBrace );
        if ( bBrace )
        {
            Advance();
        }
        else
        {
            Expect( TokenKind::KwDo, "to begin a block" );
        }

        Block Node;
        // Parameters are hard-expected between pipes, outside the Pratt
        // table, so `|` never reads as the bitwise-or operator here.
        if ( Accept( TokenKind::Pipe ) )
        {
            do
            {
                Param ParamNode;
                ParamNode.Name = InternText( Expect( TokenKind::Identifier, "as a block parameter name" ) );
                if ( Accept( TokenKind::Colon ) )
                {
                    ParamNode.DeclType = ParseType();
                }
                Node.Params.PushBack( Context.Add( std::move( ParamNode ) ) );
            } while ( Accept( TokenKind::Comma ) );
            Expect( TokenKind::Pipe, "to close block parameters" );
        }

        // The body is a fresh statement context: `do` may attach again.
        const bool Saved = bNoDoBlock;
        bNoDoBlock       = false;

        // `end` is already one of ParseStatementBlock's built-in terminators;
        // only a brace block needs the extra one.
        const TokenKind Close = bBrace ? TokenKind::RBrace : TokenKind::KwEnd;
        if ( bBrace )
        {
            ParseStatementBlock( Node.Body, Close );
        }
        else
        {
            ParseStatementBlock( Node.Body );
        }
        bNoDoBlock = Saved;
        Expect( Close, "to close block" );

        return MakeExpr( std::move( Node ), RangeSince( Begin ) );
    }

    ExprId Parser::ParseCommandCallArgs ( ExprId Callee, Core::SourceRange Start )
    {
        Call Node;
        Node.Callee = Callee;

        // Inside the argument list a trailing `do` belongs to this command
        // call, not to the last argument — suppress it there (`{` still
        // binds tightest and may attach to an argument, as in Ruby).
        const bool Saved = bNoDoBlock;
        bNoDoBlock       = true;
        do
        {
            Symbol Name;
            if ( Check( TokenKind::Identifier ) and PeekKind( 1 ) == TokenKind::Colon )
            {
                Name = InternText( Advance() );
                Advance(); // ':'
            }
            Node.Args.PushBack( ParseExpr( 0 ) );
            Node.ArgNames.PushBack( Name );
        } while ( Accept( TokenKind::Comma ) );
        bNoDoBlock = Saved;

        // This result never flows back through ParsePostfix, so claim the
        // trailing block here (`foo 1 do |y| ... end`).
        if ( Check( TokenKind::KwDo ) or Check( TokenKind::LBrace ) )
        {
            Node.BlockArg = ParseDoBlock();
        }

        return MakeExpr( std::move( Node ), RangeSince( Start.Begin ) );
    }

    ExprId Parser::ParseParenOrGroup ()
    {
        Expect( TokenKind::LParen, "to open a parenthesised expression" );
        SkipNewlines();
        // Parens delimit a fresh expression context: `do` may attach again.
        const bool Saved = bNoDoBlock;
        bNoDoBlock       = false;
        const ExprId Inner = ParseExpr( 0 );
        bNoDoBlock         = Saved;
        SkipNewlines();
        Expect( TokenKind::RParen, "to close a parenthesised expression" );
        return Inner;
    }

    ExprId Parser::ParseArrayLiteral ()
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

    ExprId Parser::ParseHashLiteral ()
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
                // Parse the key above `=>`'s binding power so the Pratt loop
                // leaves the arrow for the explicit Expect below.
                Node.Keys.PushBack( ParseExpr( InfixBinding( TokenKind::FatArrow ).Left + 1 ) );
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

    ExprId Parser::ParseStringLiteral ( const Token &Tok )
    {
        if ( !Tok.bHasInterpolation )
        {
            StringLiteral Node;
            Node.Value = Tok.Lexeme;
            return MakeExpr( std::move( Node ), Tok.Range );
        }

        const std::string_view Raw = Interner.Resolve( Tok.Lexeme );

        Interp Node;
        std::size_t LiteralStart = 0;
        std::size_t Index        = 0;
        while ( Index < Raw.size() )
        {
            if ( Raw[Index] == '#' and Index + 1 < Raw.size() and Raw[Index + 1] == '{' )
            {
                if ( Index > LiteralStart )
                {
                    StringLiteral Chunk;
                    Chunk.Value = Interner.Intern( Raw.substr( LiteralStart, Index - LiteralStart ) );
                    Node.Parts.PushBack( MakeExpr( std::move( Chunk ), Tok.Range ) );
                }

                std::size_t Cursor = Index + 2;
                int Depth          = 1;
                while ( Cursor < Raw.size() and Depth > 0 )
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

    ExprId Parser::ParseSubExpression ( std::string_view Text, Core::SourceRange Range )
    {
        Lexer SubLexer( Context.FileId(), Text, Interner, Diagnostics );
        std::vector<Token> SubTokens = SubLexer.Tokenize();
        Parser SubParser( std::move( SubTokens ), Context, Diagnostics );
        const ExprId Result = SubParser.ParseExpr( 0 );
        if ( Result.IsValid() )
        {
            return Result;
        }
        return MakeExpr( NilLiteral{}, Range );
    }

} // namespace Frontend

} // namespace Volt
