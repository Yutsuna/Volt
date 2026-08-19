#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/Frontend/Lexer/UnitTable.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{

[[nodiscard]] inline bool IsIdentStart ( char C )
{
    return ( C >= 'a' and C <= 'z' ) or ( C >= 'A' and C <= 'Z' ) or C == '_';
}

[[nodiscard]] inline bool IsIdentCont ( char C )
{
    return IsIdentStart( C ) or ( C >= '0' and C <= '9' );
}

[[nodiscard]] inline bool IsDigit ( char C )
{
    return C >= '0' and C <= '9';
}

[[nodiscard]] inline bool IsHexDigit ( char C )
{
    return IsDigit( C ) or ( C >= 'a' and C <= 'f' ) or ( C >= 'A' and C <= 'F' );
}

struct PunctEntry
{

    std::string_view Spelling;
    Volt::Frontend::TokenKind Kind;
};

// Punctuation ordered longest-first so the greedy matcher never
// stops at a proper prefix (e.g. "<<=" before "<<" before "<").
const std::vector<PunctEntry> &PunctTable ()
{
    static const std::vector<PunctEntry> Table = []
    {
        std::vector<PunctEntry> Entries = {
#define VOLT_PUNCT( Name, Spelling ) PunctEntry{ Spelling, Volt::Frontend::TokenKind::Name },
#include "Volt/Frontend/Lexer/TokenKind.inl"
        };
        std::ranges::stable_sort( Entries, [] ( const PunctEntry &A, const PunctEntry &B )
                                  { return A.Spelling.size() > B.Spelling.size(); } );
        return Entries;
    }();
    return Table;
}

} // namespace

Volt::Frontend::Lexer::Lexer ( Core::FileId InFile,
                               std::string_view InSource,
                               Core::StringInterner &InInterner,
                               Core::DiagEngine::Bag &InDiagnostics )
    : File( InFile ), Source( InSource ), Interner( InInterner ), Diagnostics( InDiagnostics )
{
}

Volt::Frontend::Token Volt::Frontend::Lexer::Make ( TokenKind Kind, std::size_t Start ) const
{
    Token Result;
    Result.Kind  = Kind;
    Result.Range = RangeFrom( Start );
    return Result;
}

Volt::Frontend::Token Volt::Frontend::Lexer::MakeText ( TokenKind Kind, std::size_t Start )
{
    Token Result;
    Result.Kind   = Kind;
    Result.Range  = RangeFrom( Start );
    Result.Lexeme = Interner.Intern( Source.substr( Start, Pos - Start ) );
    return Result;
}

void Volt::Frontend::Lexer::SkipInlineWhitespace ()
{
    while ( not AtEnd() )
    {
        const char C = Peek();
        if ( C == ' ' or C == '\t' or C == '\r' )
        {
            ++Pos;
        }
        else
        {
            break;
        }
    }
}

bool Volt::Frontend::Lexer::SkipCommentOrDoc ()
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
        while ( not AtEnd() and ( Peek() != '#' or Peek( 1 ) != '}' ) )
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
    while ( not AtEnd() and Peek() != '\n' )
    {
        ++Pos;
    }
    return true;
}

Volt::Frontend::Token Volt::Frontend::Lexer::LexNewline ( std::size_t Start )
{
    // Collapse a run of newlines (and interspersed inline whitespace /
    // comments) into a single Newline token.
    while ( not AtEnd() )
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

Volt::Frontend::Token Volt::Frontend::Lexer::LexIdentifier ( std::size_t Start )
{
    const bool bConstant = Source[Start] >= 'A' and Source[Start] <= 'Z';

    while ( not AtEnd() and IsIdentCont( Peek() ) )
    {
        ++Pos;
    }

    // Method-style suffixes: `admin?`, `save!`.
    if ( Peek() == '?' or Peek() == '!' )
    {
        ++Pos;
        // The keyword table is consulted *after* the suffix, not instead of
        // it: a predicate keyword reads the way every other predicate in the
        // language does (`trivially_destructible? T`), and an ordinary
        // `admin?` still falls through to Identifier because no manifest row
        // spells it. One lookup, no per-keyword knowledge here.
        if ( const TokenKind Suffixed = KeywordLookup( Source.substr( Start, Pos - Start ) ); Suffixed != TokenKind::Identifier )
        {
            return Make( Suffixed, Start );
        }
        return MakeText( TokenKind::Identifier, Start );
    }

    const std::string_view Text = Source.substr( Start, Pos - Start );
    const TokenKind Kw          = KeywordLookup( Text );
    if ( Kw != TokenKind::Identifier )
    {
        return Make( Kw, Start );
    }

    return MakeText( bConstant ? TokenKind::Constant : TokenKind::Identifier, Start );
}

Volt::Frontend::Token Volt::Frontend::Lexer::LexNumber ( std::size_t Start )
{
    bool bFloat = false;

    // A '_' is a digit-group separator only when a digit follows it;
    // otherwise it begins a typed suffix (`16_u64`) and must be left.
    if ( Peek() == '0' and ( Peek( 1 ) == 'x' or Peek( 1 ) == 'X' ) )
    {
        Pos += 2;
        while ( IsHexDigit( Peek() ) or ( Peek() == '_' and IsHexDigit( Peek( 1 ) ) ) )
        {
            ++Pos;
        }
    }
    else if ( Peek() == '0' and ( Peek( 1 ) == 'b' or Peek( 1 ) == 'B' ) )
    {
        Pos += 2;
        while ( Peek() == '0' or Peek() == '1' or ( Peek() == '_' and ( Peek( 1 ) == '0' or Peek( 1 ) == '1' ) ) )
        {
            ++Pos;
        }
    }
    else
    {
        while ( IsDigit( Peek() ) or ( Peek() == '_' and IsDigit( Peek( 1 ) ) ) )
        {
            ++Pos;
        }

        // Fractional part, but only if a digit follows the dot (so that
        // `1..2` ranges and `5.foo` calls keep the dot separate).
        if ( Peek() == '.' and IsDigit( Peek( 1 ) ) )
        {
            bFloat = true;
            ++Pos;
            while ( IsDigit( Peek() ) or ( Peek() == '_' and IsDigit( Peek( 1 ) ) ) )
            {
                ++Pos;
            }
        }

        if ( ( Peek() == 'e' or Peek() == 'E' ) and
             ( IsDigit( Peek( 1 ) ) or ( ( Peek( 1 ) == '+' or Peek( 1 ) == '-' ) and IsDigit( Peek( 2 ) ) ) ) )
        {
            bFloat = true;
            ++Pos;
            if ( Peek() == '+' or Peek() == '-' )
            {
                ++Pos;
            }
            while ( IsDigit( Peek() ) or ( Peek() == '_' and IsDigit( Peek( 1 ) ) ) )
            {
                ++Pos;
            }
        }
    }

    const std::size_t NumEnd    = Pos;
    const std::string_view Text = Source.substr( Start, NumEnd - Start );

    // Check for unit suffixes (e.g. `1KiB`, `1000ms`, `180deg`, `80%`, `1kHz`, `1Gbps`)
    if ( const UnitEntry *Unit = MatchUnitSuffix( Source, Pos ) )
    {
        Pos += Unit->Suffix.size();

        char CleanBuf[64];
        std::size_t CleanLen = 0;
        bool bOverflowDigits = false;
        for ( const char Ch : Text )
        {
            if ( Ch != '_' )
            {
                if ( CleanLen + 1 >= sizeof( CleanBuf ) )
                {
                    bOverflowDigits = true;
                    break;
                }
                CleanBuf[CleanLen++] = Ch;
            }
        }
        if ( bOverflowDigits )
        {
            Diagnostics.Error( RangeFrom( Start ), "number literal exceeds maximum supported length" );
            return MakeText( TokenKind::Error, Start );
        }

        char OutBuf[64];
        UnitFoldResult Folded =
            FoldUnitLiteral( std::string_view( CleanBuf, CleanLen ), bFloat, *Unit, OutBuf, sizeof( OutBuf ) );
        if ( Folded.bOverflow )
        {
            Diagnostics.Error( RangeFrom( Start ), "integer literal overflow during unit folding" );
        }

        char *OutPtr = OutBuf + Folded.WrittenSize;

        // Append optional typed suffix (`_u64`, `_i32`, `_f64`)
        if ( Peek() == '_' and ( Peek( 1 ) == 'u' or Peek( 1 ) == 'i' or Peek( 1 ) == 'f' ) )
        {
            if ( Peek( 1 ) == 'f' )
            {
                Folded.bFloat = true;
            }
            const std::size_t TypeStart = Pos;
            Pos += 2;
            while ( IsIdentCont( Peek() ) )
            {
                ++Pos;
            }
            const std::string_view TypeSuffix = Source.substr( TypeStart, Pos - TypeStart );
            const std::size_t CopyLen =
                std::min( TypeSuffix.size(), sizeof( OutBuf ) - static_cast<std::size_t>( OutPtr - OutBuf ) );
            std::memcpy( OutPtr, TypeSuffix.data(), CopyLen );
            OutPtr += CopyLen;
        }

        Token Result;
        Result.Kind   = Folded.bFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral;
        Result.Range  = RangeFrom( Start );
        Result.Lexeme = Interner.Intern( std::string_view( OutBuf, static_cast<std::size_t>( OutPtr - OutBuf ) ) );
        return Result;
    }

    // Typed suffix without unit: `_u64`, `_i32`, `_f64`, ...
    if ( Peek() == '_' and ( Peek( 1 ) == 'u' or Peek( 1 ) == 'i' or Peek( 1 ) == 'f' ) )
    {
        if ( Peek( 1 ) == 'f' )
        {
            bFloat = true;
        }
        Pos += 2;
        while ( IsIdentCont( Peek() ) )
        {
            ++Pos;
        }
        return MakeText( bFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral, Start );
    }

    // Unrecognized alphanumeric suffix attached to number (e.g. `123xyz`)
    if ( IsIdentStart( Peek() ) )
    {
        const std::size_t BadStart = Pos;
        while ( IsIdentCont( Peek() ) )
        {
            ++Pos;
        }
        const std::string_view BadSuffix = Source.substr( BadStart, Pos - BadStart );
        Diagnostics.Error( RangeFrom( Start ), "unrecognized literal suffix '" + std::string( BadSuffix ) + "'" );
        return MakeText( TokenKind::Error, Start );
    }

    return MakeText( bFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral, Start );
}

Volt::Frontend::Token Volt::Frontend::Lexer::LexString ( std::size_t Start )
{
    ++Pos; // opening quote
    bool bInterpolation = false;

    while ( not AtEnd() and Peek() != '"' )
    {
        const char C = Peek();
        if ( C == '\\' )
        {
            if ( ( Peek( 1 ) == 'x' or Peek( 1 ) == 'X' ) and IsHexDigit( Peek( 2 ) ) )
            {
                Pos += 3;
                if ( not AtEnd() and IsHexDigit( Peek() ) )
                {
                    ++Pos;
                }
            }
            else
            {
                Pos += ( Peek( 1 ) != '\0' ) ? 2U : 1U;
            }
            continue;
        }
        if ( C == '#' and Peek( 1 ) == '{' )
        {
            bInterpolation = true;
            Pos += 2;
            // Skip the interpolation body, tracking nested braces so an
            // inner '}' (or string) does not terminate it prematurely.
            int Depth = 1;
            while ( not AtEnd() and Depth > 0 )
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

Volt::Frontend::Token Volt::Frontend::Lexer::LexChar ( std::size_t Start )
{
    ++Pos; // opening quote
    if ( Peek() == '\\' )
    {
        if ( ( Peek( 1 ) == 'x' or Peek( 1 ) == 'X' ) and IsHexDigit( Peek( 2 ) ) )
        {
            Pos += 3;
            if ( not AtEnd() and IsHexDigit( Peek() ) )
            {
                ++Pos;
            }
        }
        else
        {
            Pos += ( Peek( 1 ) != '\0' ) ? 2U : 1U;
        }
    }
    else if ( not AtEnd() )
    {
        ++Pos;
    }

    if ( Peek() != '\'' )
    {
        Diagnostics.Error( RangeFrom( Start ), "unterminated or multi-character char literal" );
        return MakeText( TokenKind::Error, Start );
    }

    const std::size_t Inner = Start + 1;
    Token Result;
    Result.Kind   = TokenKind::CharLiteral;
    Result.Lexeme = Interner.Intern( Source.substr( Inner, Pos - Inner ) );
    ++Pos; // closing quote
    Result.Range = RangeFrom( Start );
    return Result;
}

Volt::Frontend::Token Volt::Frontend::Lexer::LexSymbolOrColon ( std::size_t Start )
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
        while ( not AtEnd() and IsIdentCont( Peek() ) )
        {
            ++Pos;
        }
        if ( Peek() == '?' or Peek() == '!' )
        {
            ++Pos;
        }
        return MakeText( TokenKind::SymbolLiteral, Start );
    }

    ++Pos;
    return Make( TokenKind::Colon, Start );
}

Volt::Frontend::Token Volt::Frontend::Lexer::LexPunct ( std::size_t Start )
{
    const std::string_view Rest = Source.substr( Start );
    for ( const PunctEntry &Entry : PunctTable() )
    {
        if ( Rest.starts_with( Entry.Spelling ) )
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

Volt::Frontend::Token Volt::Frontend::Lexer::Next ()
{
    for ( ;; )
    {
        SkipInlineWhitespace();

        if ( AtEnd() )
        {
            return Make( TokenKind::Eof, Pos );
        }

        const std::size_t Start = Pos;
        const char C            = Peek();

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
        if ( C == '@' and IsIdentStart( Peek( 1 ) ) )
        {
            Pos += 1;
            while ( not AtEnd() and IsIdentCont( Peek() ) )
            {
                ++Pos;
            }
            return MakeText( TokenKind::InstanceVar, Start );
        }

        return LexPunct( Start );
    }
}

std::vector<Volt::Frontend::Token> Volt::Frontend::Lexer::Tokenize ()
{
    std::vector<Token> Tokens;
    for ( ;; )
    {
        const Token Tok = Next();
        const bool bEof = Tok.Is( TokenKind::Eof );
        Tokens.push_back( Tok );
        if ( bEof )
        {
            break;
        }
    }
    return Tokens;
}
