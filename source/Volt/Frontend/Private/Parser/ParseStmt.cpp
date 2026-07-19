#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"

#include <cstdint>
#include <utility>

namespace Volt
{

namespace Frontend
{

    void Parser::ParseStatementBlock ( StmtList &Out )
    {
        SkipTerminators();
        while ( !AtEnd() && !Check( TokenKind::KwEnd ) && !Check( TokenKind::KwElse ) && !Check( TokenKind::KwElsif ) )
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
        case TokenKind::KwReturn:
            return ApplyModifiers( ParseReturn() );
        default:
            return ApplyModifiers( ParseExprOrLocalStatement() );
        }
    }

    StmtId Parser::ParseExprOrLocalStatement ()
    {
        const std::uint32_t Begin = Here();

        // `name : Type [= init]` — a typed local declaration.
        if ( Check( TokenKind::Identifier ) && PeekKind( 1 ) == TokenKind::Colon )
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

    StmtId Parser::ParseReturn ()
    {
        const std::uint32_t Begin = Here();
        Expect( TokenKind::KwReturn, "to begin a return statement" );

        Return Node;
        // A bare `return` is followed by a terminator, `end`, or a trailing
        // statement modifier (`return if c` / `return unless c`); only parse
        // a value when none of those come next.
        if ( !Check( TokenKind::Newline ) && !Check( TokenKind::Semicolon ) && !Check( TokenKind::Eof ) &&
             !Check( TokenKind::KwEnd ) && !Check( TokenKind::KwIf ) && !Check( TokenKind::KwUnless ) )
        {
            Node.Value = ParseExpr( 0 );
        }
        return MakeStmt( std::move( Node ), RangeSince( Begin ) );
    }

} // namespace Frontend

} // namespace Volt
