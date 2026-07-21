#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"

#include <cstdint>
#include <utility>

namespace Volt
{

namespace Frontend
{

    void Parser::ParseStatementBlock ( StmtList &Out, TokenKind ExtraTerminator )
    {
        SkipTerminators();
        while ( !AtEnd() and !Check( TokenKind::KwEnd ) and !Check( TokenKind::KwElse ) and !Check( TokenKind::KwElsif ) &&
                !Check( ExtraTerminator ) )
        {
            const std::size_t Before = Pos;

            if ( AtDeclaration() )
            {
                // A declaration inside a block body is unexpected here, but
                // recover gracefully rather than looping.
                const DeclId Ignored = ParseDeclaration();
                static_cast<void>( Ignored );
            }
            else
            {
                const StmtId Stmt = ParseStatement();
                if ( Stmt.IsValid() )
                {
                    Out.PushBack( Stmt );
                }
            }

            if ( Pos == Before )
            {
                Advance();
            }
            SkipTerminators();
        }
    }

    StmtId Parser::ParseStatement ()
    {
        switch ( PeekKind() )
        {
        case TokenKind::KwIf:
            return ParseIf();
        case TokenKind::KwWhile:
            return ParseWhile();
        case TokenKind::KwFor:
            return ParseFor();
        case TokenKind::KwReturn:
            return ApplyModifiers( ParseReturn() );
        case TokenKind::KwBreak:
            return ApplyModifiers( ParseBreak() );
        case TokenKind::KwNext:
            return ApplyModifiers( ParseNext() );
        default:
            return ApplyModifiers( ParseExprOrLocalStatement() );
        }
    }

    StmtId Parser::ParseExprOrLocalStatement ()
    {
        const std::uint32_t Begin = Here();

        // `name : Type [= init]` — a typed local declaration.
        if ( Check( TokenKind::Identifier ) and PeekKind( 1 ) == TokenKind::Colon )
        {
            LocalDecl Node;
            Node.Name = InternText( Advance() );
            Advance(); // ':'
            Node.DeclType = ParseType();
            if ( Accept( TokenKind::Assign ) )
            {
                Node.Init = ParseExpr( 0 );
            }
            return MakeStmt( std::move( Node ), RangeSince( Begin ) );
        }

        ExprStmt Node;
        Node.Expr = ParseExpr( 0 );
        return MakeStmt( std::move( Node ), RangeSince( Begin ) );
    }

    StmtId Parser::ApplyModifiers ( StmtId Inner )
    {
        if ( !Inner.IsValid() )
        {
            return Inner;
        }

        const std::uint32_t Begin = Here();

        if ( Accept( TokenKind::KwIf ) )
        {
            If Node;
            Node.Cond = ParseExpr( 0 );
            Node.Then.PushBack( Inner );
            return MakeStmt( std::move( Node ), RangeSince( Begin ) );
        }

        if ( Accept( TokenKind::KwUnless ) )
        {
            const ExprId Cond = ParseExpr( 0 );
            Unary Negated;
            Negated.Op      = TokenKind::Bang;
            Negated.Operand = Cond;
            If Node;
            Node.Cond = MakeExpr( std::move( Negated ), RangeSince( Begin ) );
            Node.Then.PushBack( Inner );
            return MakeStmt( std::move( Node ), RangeSince( Begin ) );
        }

        return Inner;
    }

    StmtId Parser::ParseIf ()
    {
        const std::uint32_t Begin = Here();
        Expect( TokenKind::KwIf, "to begin an if statement" );

        If Node;
        Node.Cond = ParseExpr( 0 );
        SkipTerminators();
        ParseStatementBlock( Node.Then );

        if ( Check( TokenKind::KwElsif ) )
        {
            Node.Else.PushBack( ParseElsif() );
        }
        else if ( Accept( TokenKind::KwElse ) )
        {
            SkipTerminators();
            ParseStatementBlock( Node.Else );
            Expect( TokenKind::KwEnd, "to close if statement" );
        }
        else
        {
            Expect( TokenKind::KwEnd, "to close if statement" );
        }

        return MakeStmt( std::move( Node ), RangeSince( Begin ) );
    }

    StmtId Parser::ParseElsif ()
    {
        const std::uint32_t Begin = Here();
        Expect( TokenKind::KwElsif, "to begin an elsif clause" );

        If Node;
        Node.Cond = ParseExpr( 0 );
        SkipTerminators();
        ParseStatementBlock( Node.Then );

        if ( Check( TokenKind::KwElsif ) )
        {
            Node.Else.PushBack( ParseElsif() );
        }
        else if ( Accept( TokenKind::KwElse ) )
        {
            SkipTerminators();
            ParseStatementBlock( Node.Else );
            Expect( TokenKind::KwEnd, "to close if statement" );
        }
        else
        {
            Expect( TokenKind::KwEnd, "to close if statement" );
        }

        return MakeStmt( std::move( Node ), RangeSince( Begin ) );
    }

    StmtId Parser::ParseWhile ()
    {
        const std::uint32_t Begin = Here();
        Expect( TokenKind::KwWhile, "to begin a while loop" );

        While Node;
        Node.Cond = ParseExpr( 0 );
        SkipTerminators();
        ParseStatementBlock( Node.Body );
        Expect( TokenKind::KwEnd, "to close while loop" );

        return MakeStmt( std::move( Node ), RangeSince( Begin ) );
    }

    StmtId Parser::ParseFor ()
    {
        const std::uint32_t Begin = Here();
        Expect( TokenKind::KwFor, "to begin a for loop" );

        Block BlockNode;

        do
        {
            const Token &VarTok = Expect( TokenKind::Identifier, "as a loop variable name" );
            Param ParamNode;
            ParamNode.Loc  = VarTok.Range;
            ParamNode.Name = InternText( VarTok );
            BlockNode.Params.PushBack( Context.Add( std::move( ParamNode ) ) );
        } while ( Accept( TokenKind::Comma ) );

        Expect( TokenKind::KwIn, "after for loop variable(s)" );

        const ExprId Iterable = ParseExpr( 0 );

        SkipTerminators();

        ParseStatementBlock( BlockNode.Body );

        Expect( TokenKind::KwEnd, "to close for loop" );

        const Core::SourceRange Span = RangeSince( Begin );
        const ExprId BlockExpr       = MakeExpr( std::move( BlockNode ), Span );

        Member MemberNode;
        MemberNode.Object   = Iterable;
        MemberNode.Name     = Interner.Intern( "each" );
        const ExprId Callee = MakeExpr( std::move( MemberNode ), Span );

        Call CallNode;
        CallNode.Callee       = Callee;
        CallNode.BlockArg     = BlockExpr;
        const ExprId CallExpr = MakeExpr( std::move( CallNode ), Span );

        ExprStmt Node;
        Node.Expr = CallExpr;
        return MakeStmt( std::move( Node ), Span );
    }

    StmtId Parser::ParseReturn ()
    {
        const std::uint32_t Begin = Here();
        Expect( TokenKind::KwReturn, "to begin a return statement" );

        Return Node;
        // A bare `return` is followed by a terminator, `end`, or a trailing
        // statement modifier (`return if c` / `return unless c`); only parse
        // a value when none of those come next.
        if ( !Check( TokenKind::Newline ) and !Check( TokenKind::Semicolon ) and !Check( TokenKind::Eof ) &&
             !Check( TokenKind::KwEnd ) and !Check( TokenKind::KwIf ) and !Check( TokenKind::KwUnless ) )
        {
            Node.Value = ParseExpr( 0 );
        }
        return MakeStmt( std::move( Node ), RangeSince( Begin ) );
    }

    StmtId Parser::ParseBreak ()
    {
        const std::uint32_t Begin = Here();
        Expect( TokenKind::KwBreak, "to begin a break statement" );

        Break Node;
        if ( !Check( TokenKind::Newline ) and !Check( TokenKind::Semicolon ) and !Check( TokenKind::Eof ) &&
             !Check( TokenKind::KwEnd ) and !Check( TokenKind::KwIf ) and !Check( TokenKind::KwUnless ) )
        {
            Node.Value = ParseExpr( 0 );
        }
        return MakeStmt( std::move( Node ), RangeSince( Begin ) );
    }

    StmtId Parser::ParseNext ()
    {
        const std::uint32_t Begin = Here();
        Expect( TokenKind::KwNext, "to begin a next statement" );

        Next Node;
        if ( !Check( TokenKind::Newline ) and !Check( TokenKind::Semicolon ) and !Check( TokenKind::Eof ) &&
             !Check( TokenKind::KwEnd ) and !Check( TokenKind::KwIf ) and !Check( TokenKind::KwUnless ) )
        {
            Node.Value = ParseExpr( 0 );
        }
        return MakeStmt( std::move( Node ), RangeSince( Begin ) );
    }

} // namespace Frontend

} // namespace Volt
