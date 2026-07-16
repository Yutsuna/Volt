#include "Volt/Frontend/Lexer/Lexer.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Volt
{

    namespace Frontend
    {

        namespace
        {

            [[nodiscard]] bool IsIdentStart( char C )
            {
                return ( C >= 'a' && C <= 'z' ) || ( C >= 'A' && C <= 'Z' ) || C == '_';
            }

            [[nodiscard]] bool IsIdentCont( char C )
            {
                return IsIdentStart( C ) || ( C >= '0' && C <= '9' );
            }

            [[nodiscard]] bool IsDigit( char C )
            {
                return C >= '0' && C <= '9';
            }

            [[nodiscard]] bool IsHexDigit( char C )
            {
                return IsDigit( C ) || ( C >= 'a' && C <= 'f' ) || ( C >= 'A' && C <= 'F' );
            }

            struct PunctEntry
            {

                std::string_view Spelling;
                TokenKind        Kind;
            };

            // Punctuation ordered longest-first so the greedy matcher never
            // stops at a proper prefix (e.g. "<<=" before "<<" before "<").
            const std::vector<PunctEntry>& PunctTable()
            {
                static const std::vector<PunctEntry> Table = []
                {
                    std::vector<PunctEntry> Entries = {
#define VOLT_PUNCT( Name, Spelling ) PunctEntry{ Spelling, TokenKind::Name },
#include "Volt/Frontend/Lexer/TokenKind.inl"
                    };
                    std::stable_sort( Entries.begin(), Entries.end(),
                                      []( const PunctEntry& A, const PunctEntry& B ) { return A.Spelling.size() > B.Spelling.size(); } );
                    return Entries;
                }();
                return Table;
            }

        }

        Lexer::Lexer( Core::FileId InFile, std::string_view InSource, Core::StringInterner& InInterner, Core::DiagEngine::Bag& InDiagnostics )
            : File( InFile ), Source( InSource ), Interner( InInterner ), Diagnostics( InDiagnostics )
        {
        }

        Token Lexer::Make( TokenKind Kind, std::size_t Start ) const
        {
            Token Result;
            Result.Kind  = Kind;
            Result.Range = RangeFrom( Start );
            return Result;
        }

        Token Lexer::MakeText( TokenKind Kind, std::size_t Start )
        {
            Token Result;
            Result.Kind   = Kind;
            Result.Range  = RangeFrom( Start );
            Result.Lexeme = Interner.Intern( Source.substr( Start, Pos - Start ) );
            return Result;
        }

        void Lexer::SkipInlineWhitespace()
        {
            while ( !AtEnd() )
            {
                const char C = Peek();
                if ( C == ' ' || C == '\t' || C == '\r' )
                {
                    ++Pos;
                }
                else
                {
                    break;
                }
            }
        }

        bool Lexer::SkipCommentOrDoc()
        {
            if ( Peek() != '#' )
            {
                return false;
            }

            // Doc block: #{ ... #}
            if ( Peek( 1 ) == '{' )
            {
                const std::size_t Start = Pos;
                Pos += 2;
                while ( !AtEnd() && !( Peek() == '#' && Peek( 1 ) == '}' ) )
                {
                    ++Pos;
                }
                if ( AtEnd() )
                {
                    Diagnostics.Error( RangeFrom( Start ), "unterminated doc comment (expected '#}')" );
                }
                else
                {
                    Pos += 2; // consume "#}"
                }
                return true;
            }

            // Line comment: # ... to end of line (newline stays significant).
            while ( !AtEnd() && Peek() != '\n' )
            {
                ++Pos;
            }
            return true;
        }

        Token Lexer::LexNewline( std::size_t Start )
        {
            // Collapse a run of newlines (and interspersed inline whitespace /
            // comments) into a single Newline token.
            while ( !AtEnd() )
            {
                SkipInlineWhitespace();
                if ( Peek() == '\n' )
                {
                    ++Pos;
                    continue;
                }
                if ( Peek() == '#' )
                {
                    SkipCommentOrDoc();
                    continue;
                }
                break;
            }
            return Make( TokenKind::Newline, Start );
        }

        Token Lexer::LexIdentifier( std::size_t Start )
        {
            const bool bConstant = Source[Start] >= 'A' && Source[Start] <= 'Z';

            while ( !AtEnd() && IsIdentCont( Peek() ) )
            {
                ++Pos;
            }

            // Method-style suffixes: `admin?`, `save!`.
            if ( Peek() == '?' || Peek() == '!' )
            {
                ++Pos;
                return MakeText( TokenKind::Identifier, Start );
            }

            const std::string_view Text = Source.substr( Start, Pos - Start );
            const TokenKind        Kw   = KeywordLookup( Text );
            if ( Kw != TokenKind::Identifier )
            {
                return Make( Kw, Start );
            }

            return MakeText( bConstant ? TokenKind::Constant : TokenKind::Identifier, Start );
        }

        Token Lexer::LexNumber( std::size_t Start )
        {
            bool bFloat = false;

            // A '_' is a digit-group separator only when a digit follows it;
            // otherwise it begins a typed suffix (`16_u64`) and must be left.
            if ( Peek() == '0' && ( Peek( 1 ) == 'x' || Peek( 1 ) == 'X' ) )
            {
                Pos += 2;
                while ( IsHexDigit( Peek() ) || ( Peek() == '_' && IsHexDigit( Peek( 1 ) ) ) )
                {
                    ++Pos;
                }
            }
            else if ( Peek() == '0' && ( Peek( 1 ) == 'b' || Peek( 1 ) == 'B' ) )
            {
                Pos += 2;
                while ( Peek() == '0' || Peek() == '1' || ( Peek() == '_' && ( Peek( 1 ) == '0' || Peek( 1 ) == '1' ) ) )
                {
                    ++Pos;
                }
            }
            else
            {
                while ( IsDigit( Peek() ) || ( Peek() == '_' && IsDigit( Peek( 1 ) ) ) )
                {
                    ++Pos;
                }

                // Fractional part, but only if a digit follows the dot (so that
                // `1..2` ranges and `5.foo` calls keep the dot separate).
                if ( Peek() == '.' && IsDigit( Peek( 1 ) ) )
                {
                    bFloat = true;
                    ++Pos;
                    while ( IsDigit( Peek() ) || ( Peek() == '_' && IsDigit( Peek( 1 ) ) ) )
                    {
                        ++Pos;
                    }
                }

                if ( Peek() == 'e' || Peek() == 'E' )
                {
                    bFloat = true;
                    ++Pos;
                    if ( Peek() == '+' || Peek() == '-' )
                    {
                        ++Pos;
                    }
                    while ( IsDigit( Peek() ) )
                    {
                        ++Pos;
                    }
                }
            }

            // Typed suffix: `_u64`, `_i32`, `_f64`, ...
            if ( Peek() == '_' && ( Peek( 1 ) == 'u' || Peek( 1 ) == 'i' || Peek( 1 ) == 'f' ) )
            {
                if ( Peek( 1 ) == 'f' )
                {
                    bFloat = true;
                }
                ++Pos; // consume '_'
                while ( IsIdentCont( Peek() ) )
                {
                    ++Pos;
                }
            }

            return MakeText( bFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral, Start );
        }

        Token Lexer::LexString( std::size_t Start )
        {
            ++Pos; // opening quote
            bool bInterpolation = false;

            while ( !AtEnd() && Peek() != '"' )
            {
                const char C = Peek();
                if ( C == '\\' )
                {
                    Pos += ( Peek( 1 ) != '\0' ) ? 2 : 1;
                    continue;
                }
                if ( C == '#' && Peek( 1 ) == '{' )
                {
                    bInterpolation = true;
                    Pos += 2;
                    // Skip the interpolation body, tracking nested braces so an
                    // inner '}' (or string) does not terminate it prematurely.
                    int Depth = 1;
                    while ( !AtEnd() && Depth > 0 )
                    {
                        const char D = Peek();
                        if ( D == '{' )
                        {
                            ++Depth;
                        }
                        else if ( D == '}' )
                        {
                            --Depth;
                        }
                        ++Pos;
                    }
                    continue;
                }
                ++Pos;
            }

            if ( AtEnd() )
            {
                Diagnostics.Error( RangeFrom( Start ), "unterminated string literal" );
                return MakeText( TokenKind::Error, Start );
            }

            // Intern the inner content (between the quotes), excluding them.
            Token Result;
            Result.Kind              = TokenKind::StringLiteral;
            Result.bHasInterpolation = bInterpolation;
            const std::size_t Inner  = Start + 1;
            Result.Lexeme            = Interner.Intern( Source.substr( Inner, Pos - Inner ) );
            ++Pos; // closing quote
            Result.Range = RangeFrom( Start );
            return Result;
        }

        Token Lexer::LexChar( std::size_t Start )
        {
            ++Pos; // opening quote
            if ( Peek() == '\\' )
            {
                Pos += ( Peek( 1 ) != '\0' ) ? 2 : 1;
            }
            else if ( !AtEnd() )
            {
                ++Pos;
            }

            if ( Peek() != '\'' )
            {
                Diagnostics.Error( RangeFrom( Start ), "unterminated or multi-character char literal" );
                return MakeText( TokenKind::Error, Start );
            }

            const std::size_t Inner = Start + 1;
            Token             Result;
            Result.Kind   = TokenKind::CharLiteral;
            Result.Lexeme = Interner.Intern( Source.substr( Inner, Pos - Inner ) );
            ++Pos; // closing quote
            Result.Range = RangeFrom( Start );
            return Result;
        }

        Token Lexer::LexSymbolOrColon( std::size_t Start )
        {
            if ( Peek( 1 ) == ':' )
            {
                Pos += 2;
                return Make( TokenKind::ColonColon, Start );
            }

            // `:name` in value position is a symbol literal. A bare `:` (type
            // annotation, ternary, named arg) is handled by the punct matcher;
            // we only treat it as a symbol when an identifier follows directly.
            if ( IsIdentStart( Peek( 1 ) ) )
            {
                Pos += 1; // ':'
                while ( !AtEnd() && IsIdentCont( Peek() ) )
                {
                    ++Pos;
                }
                if ( Peek() == '?' || Peek() == '!' )
                {
                    ++Pos;
                }
                return MakeText( TokenKind::SymbolLiteral, Start );
            }

            ++Pos;
            return Make( TokenKind::Colon, Start );
        }

        Token Lexer::LexPunct( std::size_t Start )
        {
            const std::string_view Rest = Source.substr( Start );
            for ( const PunctEntry& Entry : PunctTable() )
            {
                if ( Rest.substr( 0, Entry.Spelling.size() ) == Entry.Spelling )
                {
                    Pos = Start + Entry.Spelling.size();
                    return Make( Entry.Kind, Start );
                }
            }

            // Unknown byte: report and skip one character to keep going.
            ++Pos;
            Diagnostics.Error( RangeFrom( Start ), "unexpected character '" + std::string{ Source.substr( Start, 1 ) } + "'" );
            return Make( TokenKind::Error, Start );
        }

        Token Lexer::Next()
        {
            for ( ;; )
            {
                SkipInlineWhitespace();

                if ( AtEnd() )
                {
                    return Make( TokenKind::Eof, Pos );
                }

                const std::size_t Start = Pos;
                const char        C     = Peek();

                if ( C == '\n' )
                {
                    ++Pos;
                    return LexNewline( Start );
                }

                if ( C == '#' )
                {
                    SkipCommentOrDoc();
                    continue;
                }

                if ( IsIdentStart( C ) )
                {
                    return LexIdentifier( Start );
                }

                if ( IsDigit( C ) )
                {
                    return LexNumber( Start );
                }

                if ( C == '"' )
                {
                    return LexString( Start );
                }

                if ( C == '\'' )
                {
                    return LexChar( Start );
                }

                if ( C == ':' )
                {
                    return LexSymbolOrColon( Start );
                }

                // `@name` is an instance variable, but `@[` (annotation) and a
                // lone `@` fall through to the punctuation matcher.
                if ( C == '@' && IsIdentStart( Peek( 1 ) ) )
                {
                    Pos += 1;
                    while ( !AtEnd() && IsIdentCont( Peek() ) )
                    {
                        ++Pos;
                    }
                    return MakeText( TokenKind::InstanceVar, Start );
                }

                return LexPunct( Start );
            }
        }

        std::vector<Token> Lexer::Tokenize()
        {
            std::vector<Token> Tokens;
            for ( ;; )
            {
                Token Tok = Next();
                const bool bEof = Tok.Is( TokenKind::Eof );
                Tokens.push_back( Tok );
                if ( bEof )
                {
                    break;
                }
            }
            return Tokens;
        }

    }

}
