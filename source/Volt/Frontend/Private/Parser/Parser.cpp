#include "Volt/Frontend/Parser/Parser.hpp"

#include <string>
#include <utility>

// NOLINTNEXTLINE
Volt::Frontend::Parser::Parser ( std::vector<Token> InTokens,
                                 AstContext &InContext,
                                 Core::DiagEngine::Bag &InDiagnostics,
                                 std::string_view InSource )
    : Tokens( std::move( InTokens ) ), Context( InContext ), Diagnostics( InDiagnostics ), Interner( InContext.Strings() ),
      Source( InSource )
{
    if ( Tokens.empty() )
    {
        Tokens.push_back( Token{ TokenKind::Eof, {}, {}, false } );
    }
}

std::string_view Volt::Frontend::Parser::Spelling ( const Token &Tok ) const
{
    if ( Tok.Lexeme.IsValid() )
    {
        return Interner.Resolve( Tok.Lexeme );
    }
    return TokenSpelling( Tok.Kind );
}

Volt::Frontend::Symbol Volt::Frontend::Parser::InternText ( const Token &Tok ) const
{
    if ( Tok.Lexeme.IsValid() )
    {
        return Tok.Lexeme;
    }
    return Interner.Intern( TokenSpelling( Tok.Kind ) );
}

void Volt::Frontend::Parser::ReportHere ( std::string Message )
{
    ReportAt( Peek().Range, std::move( Message ) );
}

void Volt::Frontend::Parser::ReportAt ( Core::SourceRange Range, std::string Message )
{
    Diagnostics.Error( Range, std::move( Message ) );
}

const Volt::Frontend::Token &Volt::Frontend::Parser::Expect ( TokenKind Kind, std::string_view Where )
{
    if ( Check( Kind ) )
    {
        return Advance();
    }

    std::string Message           = "expected ";
    const std::string_view Wanted = TokenSpelling( Kind );
    if ( Wanted.empty() )
    {
        Message += TokenName( Kind );
    }
    else
    {
        Message += '\'';
        Message += Wanted;
        Message += '\'';
    }
    Message += " ";
    Message += Where;
    Message += ", found '";
    Message += Spelling( Peek() );
    Message += '\'';
    ReportHere( std::move( Message ) );
    return Peek();
}

void Volt::Frontend::Parser::RecoverToStatement ()
{
    while ( !AtEnd() )
    {
        if ( Check( TokenKind::Newline ) or Check( TokenKind::Semicolon ) )
        {
            return;
        }
        if ( Check( TokenKind::KwEnd ) )
        {
            return;
        }
        Advance();
    }
}

void Volt::Frontend::Parser::ParseFile ()
{
    SkipTerminators();
    while ( !AtEnd() )
    {
        const std::size_t Before = Pos;

        if ( AtDeclaration() )
        {
            const DeclId Decl = ParseDeclaration();
            if ( Decl.IsValid() )
            {
                Context.TopDecls.push_back( Decl );
            }
            DrainAnnotations( Context.TopDecls );
        }
        else
        {
            const StmtId Stmt = ParseStatement();
            if ( Stmt.IsValid() )
            {
                Context.TopStmts.push_back( Stmt );
            }
        }

        // Guarantee forward progress even if a sub-parser stalled.
        if ( Pos == Before )
        {
            Advance();
        }
        SkipTerminators();
    }
}
