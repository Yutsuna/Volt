// LineState.cpp — block/bracket/continuation tracking over the token stream.

#include "Volt/ReplCore/LineState.hpp"

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace
{

using Volt::Frontend::TokenKind;

// Keywords that open a block terminated by `end` wherever they appear.
[[nodiscard]] bool AlwaysOpens ( const TokenKind Kind )
{
    switch ( Kind )
    {
    case TokenKind::KwDef:
    case TokenKind::KwMacro:
    case TokenKind::KwClass:
    case TokenKind::KwStruct:
    case TokenKind::KwEnum:
    case TokenKind::KwMixin:
    case TokenKind::KwModule:
    case TokenKind::KwComponent:
    case TokenKind::KwCircuit:
    case TokenKind::KwCase:
    case TokenKind::KwBegin:
    case TokenKind::KwDo:
    case TokenKind::KwFor:
        return true;
    default:
        return false;
    }
}

// Keywords that open a block only where an expression may begin. `x = 1 if
// cond` is a modifier and closes nothing; `if cond` on its own line wants an
// `end`, and so does the `if` in `x = if cond` — nothing precedes it that it
// could modify. Distinguished by position for the same reason Ruby
// distinguishes them there: the spelling alone cannot tell the two apart.
[[nodiscard]] bool OpensAtExprHead ( const TokenKind Kind )
{
    switch ( Kind )
    {
    case TokenKind::KwIf:
    case TokenKind::KwUnless:
    case TokenKind::KwWhile:
    case TokenKind::KwUntil:
        return true;
    default:
        return false;
    }
}

// A line that ends on one of these has nothing to bind on the right yet, so
// the statement cannot be over. Assignment and the binary operators only —
// `x++` is complete, and so is a bare `!`.
[[nodiscard]] bool WantsARightHandSide ( const TokenKind Kind )
{
    switch ( Kind )
    {
    case TokenKind::Assign:
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::Pow:
    case TokenKind::Comma:
    case TokenKind::Dot:
    case TokenKind::AmpDot:
    case TokenKind::ColonColon:
    case TokenKind::AndAnd:
    case TokenKind::OrOr:
    case TokenKind::KwAnd:
    case TokenKind::KwOr:
    case TokenKind::KwNot:
    case TokenKind::PipeGreater:
    case TokenKind::LessPipe:
    case TokenKind::Arrow:
    case TokenKind::FatArrow:
    case TokenKind::Amp:
    case TokenKind::Pipe:
    case TokenKind::Caret:
    case TokenKind::Shl:
    case TokenKind::Shr:
    case TokenKind::EqEq:
    case TokenKind::NotEq:
    case TokenKind::TripleEq:
    case TokenKind::Lt:
    case TokenKind::Gt:
    case TokenKind::Le:
    case TokenKind::Ge:
    case TokenKind::Spaceship:
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
    case TokenKind::KwThen:
    case TokenKind::KwElse:
    case TokenKind::KwElsif:
    case TokenKind::KwEnsure:
        return true;
    default:
        return false;
    }
}

} // namespace

Volt::Repl::ELineState Volt::Repl::Classify ( const std::string_view Accumulated )
{
    // Both are thrown away. The interner is where the lexer puts identifier
    // text and the bag is where it puts complaints; a classification wants
    // neither, only the shape of the stream.
    Volt::Core::StringInterner Interner;
    Volt::Core::DiagEngine::Bag Bag = Volt::Core::DiagEngine::MakeBag();

    Volt::Frontend::Lexer Scanner( Volt::Core::FileId{}, Accumulated, Interner, Bag );
    const std::vector<Volt::Frontend::Token> Tokens = Scanner.Tokenize();

    std::ptrdiff_t Blocks   = 0;
    std::ptrdiff_t Brackets = 0;

    // True at the very start and after every newline or `;` — where a
    // statement may begin.
    bool bAtStatementHead = true;

    TokenKind Last = TokenKind::Newline;

    for ( const Volt::Frontend::Token &Tok : Tokens )
    {
        switch ( Tok.Kind )
        {
        case TokenKind::Eof:
            continue;

        case TokenKind::Newline:
        case TokenKind::Semicolon:
            bAtStatementHead = true;
            continue;

        case TokenKind::LParen:
        case TokenKind::LBracket:
        case TokenKind::LBrace:
            ++Brackets;
            break;

        case TokenKind::RParen:
        case TokenKind::RBracket:
        case TokenKind::RBrace:
            --Brackets;
            break;

        case TokenKind::KwEnd:
            --Blocks;
            break;

        // An unterminated literal. The lexer has already consumed the rest of
        // the input looking for a closing quote, so there is nothing after it
        // and another line is exactly what it needs.
        case TokenKind::Error:
            return ELineState::NeedsMore;

        default:
            // An expression may begin at a statement head, and also right after
            // an operator still waiting for its right-hand side: in `val = if
            // 10 > 5` the `if` is the value being assigned, and a REPL that
            // read that line as complete would evaluate `val = if 10 > 5`,
            // then `"greater"`, then `else` — each on its own, none of them
            // what was written.
            if ( AlwaysOpens( Tok.Kind ) or
                 ( ( bAtStatementHead or WantsARightHandSide( Last ) ) and OpensAtExprHead( Tok.Kind ) ) )
            {
                ++Blocks;
            }
            break;
        }

        Last             = Tok.Kind;
        bAtStatementHead = false;
    }

    // Negative depth is a stray `end` or a stray `)`. That is a syntax error,
    // not an unfinished statement, and the compiler says so far better than a
    // prompt that simply never returns.
    if ( Blocks > 0 or Brackets > 0 )
    {
        return ELineState::NeedsMore;
    }

    return WantsARightHandSide( Last ) ? ELineState::NeedsMore : ELineState::Complete;
}

bool Volt::Repl::ContinuesPrevious ( const std::string_view Line )
{
    Volt::Core::StringInterner Interner;
    Volt::Core::DiagEngine::Bag Bag = Volt::Core::DiagEngine::MakeBag();

    Volt::Frontend::Lexer Scanner( Volt::Core::FileId{}, Line, Interner, Bag );

    for ( const Volt::Frontend::Token &Tok : Scanner.Tokenize() )
    {
        switch ( Tok.Kind )
        {
        // What a blank line and a comment-only line both lex to. Neither
        // continues anything: a statement that ended is still ended.
        case TokenKind::Newline:
            continue;

        case TokenKind::Dot:
        case TokenKind::AmpDot:
        case TokenKind::PipeGreater:
        case TokenKind::LessPipe:
        case TokenKind::ColonColon:
        case TokenKind::Shr:
        case TokenKind::Shl:
        case TokenKind::Comma:
            return true;

        default:
            return false;
        }
    }
    return false;
}
