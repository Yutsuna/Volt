#include "Volt/Frontend/Parser/Parser.hpp"

#include <string>
#include <utility>

namespace Volt
{

namespace Frontend
{

    Parser::Parser ( std::vector<Token> InTokens,
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

    std::string_view Parser::Spelling ( const Token &Tok ) const
    {
        if ( Tok.Lexeme.IsValid() )
        {
            return Interner.Resolve( Tok.Lexeme );
        }
        return TokenSpelling( Tok.Kind );
    }

    Symbol Parser::InternText ( const Token &Tok ) const
    {
        if ( Tok.Lexeme.IsValid() )
        {
            return Tok.Lexeme;
        }
        return Interner.Intern( TokenSpelling( Tok.Kind ) );
    }

    void Parser::ReportHere ( std::string Message )
    {
        ReportAt( Peek().Range, std::move( Message ) );
    }

    void Parser::ReportAt ( Core::SourceRange Range, std::string Message )
    {
        Diagnostics.Error( Range, std::move( Message ) );
    }

    const Token &Parser::Expect ( TokenKind Kind, std::string_view Where )
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

    void Parser::RecoverToStatement ()
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

    void Parser::ParseFile ()
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

} // namespace Frontend

} // namespace Volt
