// VoltHighlighter.cpp — Volt source through the compiler's own lexer.
//
// The mapping from token kind to palette role is a static table generated from
// TokenKind.inl, not a switch. That is deliberate: a kind added to the language
// with no row here compiles, and paints as ordinary text. A switch would either
// stop compiling (a language change blocked on a REPL detail) or need a
// default arm that silently swallows the omission — the table makes the
// omission visible as grey text and nothing worse.
//
// Comments never reach the token stream: the lexer skips them, and a Newline
// token can swallow a run of them whole. So everything *between* tokens is
// re-scanned here for `#` and painted, which is also what makes an unfinished
// `#{ ... #}` block colour while it is being typed.

#include "Volt/ReplSyntax/Highlighter.hpp"

#include "SpanWriter.hpp"

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace
{

using Volt::Frontend::TokenKind;
using Volt::Repl::Doc::EPaletteRole;

// One row per token kind, in TokenKind.inl order. Everything not named below
// falls through to the operator/punctuation defaults at the bottom.
[[nodiscard]] constexpr EPaletteRole DefaultRoleOf ( const TokenKind Kind )
{
    switch ( Kind )
    {
    case TokenKind::Identifier:
        return EPaletteRole::Identifier;
    case TokenKind::Constant:
        return EPaletteRole::TypeName;
    case TokenKind::InstanceVar:
    case TokenKind::IvarInterp:
        return EPaletteRole::Identifier;
    case TokenKind::IntLiteral:
    case TokenKind::FloatLiteral:
        return EPaletteRole::Number;
    case TokenKind::StringLiteral:
    case TokenKind::CharLiteral:
    case TokenKind::CommandLiteral:
        return EPaletteRole::StringLiteral;
    case TokenKind::SymbolLiteral:
        return EPaletteRole::Symbol;
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
    case TokenKind::KwNil:
        return EPaletteRole::BoolNil;
    case TokenKind::LParen:
    case TokenKind::RParen:
    case TokenKind::LBracket:
    case TokenKind::RBracket:
    case TokenKind::LBrace:
    case TokenKind::RBrace:
    case TokenKind::Comma:
    case TokenKind::Semicolon:
    case TokenKind::Dot:
    case TokenKind::ColonColon:
    case TokenKind::Colon:
        return EPaletteRole::Punctuation;
    case TokenKind::Eof:
    case TokenKind::Newline:
        return EPaletteRole::Default;
    case TokenKind::Error:
        return EPaletteRole::Error;
    default:
        break;
    }

    // Keywords are a category the .inl already draws; asking it is what keeps
    // this from listing sixty rows that would go stale.
    if ( Volt::Frontend::IsKeyword( Kind ) )
    {
        return EPaletteRole::Keyword;
    }
    return EPaletteRole::Operator;
}

// Filled once, indexed by kind. A table rather than a call so the hot path —
// a repaint on every keystroke — is a load.
[[nodiscard]] const std::array<EPaletteRole, static_cast<std::size_t>( TokenKind::Count )> &RoleTable ()
{
    static const auto Table = [] ()
    {
        std::array<EPaletteRole, static_cast<std::size_t>( TokenKind::Count )> Rows{};
        for ( std::size_t Index = 0; Index < Rows.size(); ++Index )
        {
            Rows[Index] = DefaultRoleOf( static_cast<TokenKind>( Index ) );
        }
        return Rows;
    }();
    return Table;
}

// Whitespace and comments, the only two things that live between tokens.
// Everything from a `#` to the end of the region — or the end of the line for
// a `#` that is not a `#{` block — is comment.
void PaintTrivia ( Volt::Repl::Syntax::SpanWriter &Out, const std::string_view Text )
{
    std::size_t Cursor = 0;
    while ( Cursor < Text.size() )
    {
        const std::size_t Hash = Text.find( '#', Cursor );
        if ( Hash == std::string_view::npos )
        {
            Out.Emit( Text.substr( Cursor ), EPaletteRole::Default );
            return;
        }

        Out.Emit( Text.substr( Cursor, Hash - Cursor ), EPaletteRole::Default );

        std::size_t End = 0;
        if ( Hash + 1 < Text.size() and Text[Hash + 1] == '{' )
        {
            // A doc block runs to `#}`, or to the end of what has been typed
            // so far — an unfinished one still colours, which is the whole
            // reason this repaints on every keystroke.
            const std::size_t Close = Text.find( "#}", Hash + 2 );
            End                     = Close == std::string_view::npos ? Text.size() : Close + 2;
        }
        else
        {
            const std::size_t Eol = Text.find( '\n', Hash );
            End                   = Eol == std::string_view::npos ? Text.size() : Eol;
        }

        Out.Emit( Text.substr( Hash, End - Hash ), EPaletteRole::Comment );
        Cursor = End;
    }
}

// The kind of the token before this one, skipping newlines: what decides
// whether an identifier is being *declared* as a function.
[[nodiscard]] TokenKind PreviousSignificant ( const std::vector<Volt::Frontend::Token> &Tokens, const std::size_t Index )
{
    for ( std::size_t Back = Index; Back > 0; --Back )
    {
        const TokenKind Kind = Tokens[Back - 1].Kind;
        if ( Kind != TokenKind::Newline )
        {
            return Kind;
        }
    }
    return TokenKind::Newline;
}

// An identifier's role, refined by the two things a token stream alone can
// answer: `def foo` declares a function, and `foo(` calls one. Anything the
// session knows better — that `foo` names a type, say — comes from the hook.
[[nodiscard]] EPaletteRole
RefineIdentifier ( const std::vector<Volt::Frontend::Token> &Tokens, const std::size_t Index, const EPaletteRole Lexical )
{
    if ( Lexical != EPaletteRole::Identifier )
    {
        return Lexical;
    }

    if ( PreviousSignificant( Tokens, Index ) == TokenKind::KwDef )
    {
        return EPaletteRole::FunctionName;
    }
    if ( Index + 1 < Tokens.size() and Tokens[Index + 1].Kind == TokenKind::LParen )
    {
        return EPaletteRole::FunctionName;
    }
    return Lexical;
}

// The scan, shared by the document and the single-line entry points.
void Scan ( Volt::Repl::Syntax::SpanWriter &Out, const std::string_view Text, const Volt::Repl::Syntax::SemanticHook &Known )
{
    // Throwaway both: the interner dies with this call, and a half-typed line
    // produces diagnostics nobody will ever render.
    Volt::Core::StringInterner Interner;
    Volt::Core::DiagEngine::Bag Bag = Volt::Core::DiagEngine::MakeBag();

    Volt::Frontend::Lexer Scanner( Volt::Core::FileId{}, Text, Interner, Bag );
    const std::vector<Volt::Frontend::Token> Tokens = Scanner.Tokenize();

    std::size_t Cursor = 0;
    for ( std::size_t Index = 0; Index < Tokens.size(); ++Index )
    {
        const Volt::Frontend::Token &Tok = Tokens[Index];

        const std::size_t Begin = Tok.Range.Begin;
        const std::size_t End   = Tok.Range.End;
        if ( Begin > Text.size() or End > Text.size() or End < Begin )
        {
            continue;
        }

        if ( Begin > Cursor )
        {
            PaintTrivia( Out, Text.substr( Cursor, Begin - Cursor ) );
        }

        const std::string_view Lexeme = Text.substr( Begin, End - Begin );

        // A Newline token swallows the blank lines and the comments between
        // them, so its own text is trivia rather than a token.
        if ( Tok.Kind == TokenKind::Newline )
        {
            PaintTrivia( Out, Lexeme );
            Cursor = End;
            continue;
        }

        EPaletteRole Role = RoleTable()[static_cast<std::size_t>( Tok.Kind )];

        // An unterminated literal is an Error token whose text still starts
        // with its opening delimiter. Painting it red would make every string
        // flash while it is being typed; painting it as what it is becoming is
        // what an editor does.
        if ( Tok.Kind == TokenKind::Error and not Lexeme.empty() and
             ( Lexeme.front() == '"' or Lexeme.front() == '\'' or Lexeme.front() == '`' ) )
        {
            Role = EPaletteRole::StringLiteral;
        }

        Role = RefineIdentifier( Tokens, Index, Role );
        if ( Known and
             ( Role == EPaletteRole::Identifier or Role == EPaletteRole::TypeName or Role == EPaletteRole::FunctionName ) )
        {
            Role = Known( Lexeme, Role );
        }

        Out.Emit( Lexeme, Role );
        Cursor = End;
    }

    if ( Cursor < Text.size() )
    {
        PaintTrivia( Out, Text.substr( Cursor ) );
    }
}

} // namespace

Volt::Repl::Doc::EPaletteRole Volt::Repl::Syntax::RoleOf ( const Frontend::TokenKind Kind )
{
    const auto Index = static_cast<std::size_t>( Kind );
    return Index < RoleTable().size() ? RoleTable()[Index] : Doc::EPaletteRole::Default;
}

Volt::Repl::Doc::Document
Volt::Repl::Syntax::HighlightVolt ( const std::string_view Text, const Doc::Palette &Theme, const SemanticHook &Known )
{
    SpanWriter Out( Theme );
    Scan( Out, Text, Known );
    return Out.Finish();
}

Volt::Repl::Doc::Line
Volt::Repl::Syntax::HighlightVoltLine ( const std::string_view Text, const Doc::Palette &Theme, const SemanticHook &Known )
{
    SpanWriter Out( Theme );
    Scan( Out, Text, Known );
    return Out.FinishLine();
}
