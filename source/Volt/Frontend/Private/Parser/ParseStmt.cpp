#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"

#include <cstdint>

void Volt::Frontend::Parser::ParseStatementBlock ( StmtList &Out, TokenKind ExtraTerminator )
{
    SkipTerminators();
    while ( not AtEnd() and not Check( TokenKind::KwEnd ) and not Check( TokenKind::KwElse ) and
            not Check( TokenKind::KwElsif ) and not Check( TokenKind::KwRescue ) and not Check( TokenKind::KwEnsure ) and
            not Check( ExtraTerminator ) )
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

Volt::Frontend::StmtId Volt::Frontend::Parser::ParseStatement ()
{
    switch ( PeekKind() )
    {
    // No `KwIf` / `KwUnless` arm: both are expressions, so they arrive through
    // the default one and come back wrapped in an ExprStmt — the same route
    // `case` and `begin` already take.
    case TokenKind::KwWhile:
        return ParseWhile();
    case TokenKind::KwUntil:
        return ParseUntil();
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

Volt::Frontend::StmtId Volt::Frontend::Parser::ParseExprOrLocalStatement ()
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
        return MakeStmt( Node, RangeSince( Begin ) );
    }

    ExprStmt Node;
    Node.Expr = ParseExpr( 0 );
    return MakeStmt( Node, RangeSince( Begin ) );
}

Volt::Frontend::StmtId Volt::Frontend::Parser::WrapExpr ( ExprId Expr, Core::SourceRange Span )
{
    ExprStmt Node;
    Node.Expr = Expr;
    return MakeStmt( Node, Span );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::NegateCond ( ExprId Cond, Core::SourceRange Span )
{
    Unary Negated;
    Negated.Op      = TokenKind::KwNot;
    Negated.Operand = Cond;
    return MakeExpr( Negated, Span );
}

Volt::Frontend::StmtId Volt::Frontend::Parser::ApplyModifiers ( StmtId Inner )
{
    if ( not Inner.IsValid() )
    {
        return Inner;
    }

    const std::uint32_t Begin = Here();

    if ( Accept( TokenKind::KwIf ) )
    {
        If Node;
        Node.Cond = ParseExpr( 0 );
        Node.Then.PushBack( Inner );
        const Core::SourceRange Span = RangeSince( Begin );
        return WrapExpr( MakeExpr( Node, Span ), Span );
    }

    if ( Accept( TokenKind::KwUnless ) )
    {
        const ExprId Cond            = ParseExpr( 0 );
        const Core::SourceRange Span = RangeSince( Begin );
        If Node;
        Node.Cond = NegateCond( Cond, Span );
        Node.Then.PushBack( Inner );
        return WrapExpr( MakeExpr( Node, Span ), Span );
    }

    if ( Accept( TokenKind::KwUntil ) )
    {
        const ExprId Cond            = ParseExpr( 0 );
        const Core::SourceRange Span = RangeSince( Begin );
        While Node;
        Node.Cond = NegateCond( Cond, Span );
        Node.Body.PushBack( Inner );
        Node.bPostTest = WrapsBeginBlock( Inner );
        return MakeStmt( Node, Span );
    }

    // `stmt while cond` — the mirror of the `until` modifier, and the only
    // form that can express a do-while together with `begin ... end`.
    if ( Accept( TokenKind::KwWhile ) )
    {
        While Node;
        Node.Cond = ParseExpr( 0 );
        Node.Body.PushBack( Inner );
        Node.bPostTest = WrapsBeginBlock( Inner );
        return MakeStmt( Node, RangeSince( Begin ) );
    }

    return Inner;
}

// The tail every conditional shares: an optional `then`, the consequent, and
// whichever of `elsif` / `else` / `end` closes it. `then` is accepted but never
// required — both spellings are the same tree, so nothing downstream can tell
// `if c then x end` from `if c \n x end`.
//
// NOLINTNEXTLINE(misc-no-recursion)
void Volt::Frontend::Parser::ParseConditionalTail ( If &Node, const char *CloseHint )
{
    static_cast<void>( Accept( TokenKind::KwThen ) );
    SkipTerminators();
    ParseStatementBlock( Node.Then );

    if ( Check( TokenKind::KwElsif ) )
    {
        // An `elsif` is a nested If in the Else branch (Expr.hpp), and it
        // consumes the single `end` that closes the whole chain.
        const std::uint32_t ElsifBegin = Here();
        const ExprId Nested            = ParseElsif();
        Node.Else.PushBack( WrapExpr( Nested, RangeSince( ElsifBegin ) ) );
        return;
    }

    if ( Accept( TokenKind::KwElse ) )
    {
        SkipTerminators();
        ParseStatementBlock( Node.Else );
    }
    Expect( TokenKind::KwEnd, CloseHint );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseIf ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwIf, "to begin an if statement" );

    If Node;
    Node.Cond = ParseExpr( 0 );
    ParseConditionalTail( Node, "to close if statement" );

    return MakeExpr( Node, RangeSince( Begin ) );
}

// `unless c` is `if not c` — same node, negated condition, so `else` and the
// value position come for free.
Volt::Frontend::ExprId Volt::Frontend::Parser::ParseUnless ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwUnless, "to begin an unless statement" );

    const ExprId Cond = ParseExpr( 0 );

    If Node;
    Node.Cond = NegateCond( Cond, RangeSince( Begin ) );
    ParseConditionalTail( Node, "to close unless statement" );

    return MakeExpr( Node, RangeSince( Begin ) );
}

// NOLINTNEXTLINE(misc-no-recursion)
Volt::Frontend::ExprId Volt::Frontend::Parser::ParseElsif ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwElsif, "to begin an elsif clause" );

    If Node;
    Node.Cond = ParseExpr( 0 );
    ParseConditionalTail( Node, "to close if statement" );

    return MakeExpr( Node, RangeSince( Begin ) );
}

bool Volt::Frontend::Parser::WrapsBeginBlock ( StmtId Inner ) const
{
    if ( not Inner.IsValid() )
    {
        return false;
    }
    const auto *Stmt = std::get_if<ExprStmt>( &Context.Stmt( Inner ) );
    return Stmt != nullptr and Stmt->Expr.IsValid() and std::holds_alternative<BeginExpr>( Context.Expr( Stmt->Expr ) );
}

Volt::Frontend::StmtId Volt::Frontend::Parser::ParseWhile ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwWhile, "to begin a while loop" );

    While Node;
    Node.Cond = ParseExpr( 0 );
    SkipTerminators();
    ParseStatementBlock( Node.Body );
    Expect( TokenKind::KwEnd, "to close while loop" );

    return MakeStmt( Node, RangeSince( Begin ) );
}

Volt::Frontend::StmtId Volt::Frontend::Parser::ParseUntil ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwUntil, "to begin an until loop" );

    const ExprId RawCond = ParseExpr( 0 );
    Unary Negated;
    Negated.Op      = TokenKind::KwNot;
    Negated.Operand = RawCond;

    While Node;
    Node.Cond = MakeExpr( Negated, RangeSince( Begin ) );
    SkipTerminators();
    ParseStatementBlock( Node.Body );
    Expect( TokenKind::KwEnd, "to close until loop" );

    return MakeStmt( Node, RangeSince( Begin ) );
}

Volt::Frontend::StmtId Volt::Frontend::Parser::ParseFor ()
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
        BlockNode.Params.PushBack( Context.Add( ParamNode ) );
    } while ( Accept( TokenKind::Comma ) );

    Expect( TokenKind::KwIn, "after for loop variable(s)" );

    const ExprId Iterable = ParseExpr( 0 );

    SkipTerminators();

    ParseStatementBlock( BlockNode.Body );

    Expect( TokenKind::KwEnd, "to close for loop" );

    const Core::SourceRange Span = RangeSince( Begin );
    const ExprId BlockExpr       = MakeExpr( BlockNode, Span );

    Member MemberNode;
    MemberNode.Object   = Iterable;
    MemberNode.Name     = Interner.Intern( "each" );
    const ExprId Callee = MakeExpr( MemberNode, Span );

    Call CallNode;
    CallNode.Callee       = Callee;
    CallNode.BlockArg     = BlockExpr;
    const ExprId CallExpr = MakeExpr( CallNode, Span );

    ExprStmt Node;
    Node.Expr = CallExpr;
    return MakeStmt( Node, Span );
}

Volt::Frontend::StmtId Volt::Frontend::Parser::ParseReturn ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwReturn, "to begin a return statement" );

    Return Node;
    // A bare `return` is followed by a terminator, `end`, or a trailing
    // statement modifier (`return if c` / `return unless c`); only parse
    // a value when none of those come next.
    if ( not Check( TokenKind::Newline ) and not Check( TokenKind::Semicolon ) and not Check( TokenKind::Eof ) and
         not Check( TokenKind::KwEnd ) and not Check( TokenKind::KwIf ) and not Check( TokenKind::KwUnless ) and
         not Check( TokenKind::KwUntil ) and not Check( TokenKind::KwWhile ) )
    {
        Node.Value = ParseExpr( 0 );
    }
    return MakeStmt( Node, RangeSince( Begin ) );
}

Volt::Frontend::StmtId Volt::Frontend::Parser::ParseBreak ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwBreak, "to begin a break statement" );

    Break Node;
    if ( not Check( TokenKind::Newline ) and not Check( TokenKind::Semicolon ) and not Check( TokenKind::Eof ) and
         not Check( TokenKind::KwEnd ) and not Check( TokenKind::KwIf ) and not Check( TokenKind::KwUnless ) and
         not Check( TokenKind::KwUntil ) and not Check( TokenKind::KwWhile ) )
    {
        Node.Value = ParseExpr( 0 );
    }
    return MakeStmt( Node, RangeSince( Begin ) );
}

Volt::Frontend::StmtId Volt::Frontend::Parser::ParseNext ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwNext, "to begin a next statement" );

    Next Node;
    if ( not Check( TokenKind::Newline ) and not Check( TokenKind::Semicolon ) and not Check( TokenKind::Eof ) and
         not Check( TokenKind::KwEnd ) and not Check( TokenKind::KwIf ) and not Check( TokenKind::KwUnless ) and
         not Check( TokenKind::KwUntil ) and not Check( TokenKind::KwWhile ) )
    {
        Node.Value = ParseExpr( 0 );
    }
    return MakeStmt( Node, RangeSince( Begin ) );
}
