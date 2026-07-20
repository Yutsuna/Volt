#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"
#include "Volt/Core/Support/StringInterner.hpp"

#include <cstdint>
#include <string_view>

namespace Volt
{

namespace Frontend
{

    /// Every token kind, generated from TokenKind.inl.
    enum class TokenKind : std::uint8_t
    {

#define VOLT_TOKEN( Name ) Name,
#define VOLT_PUNCT( Name, Spelling ) Name,
#define VOLT_KEYWORD( Name, Spelling ) Name,
#include "Volt/Frontend/Lexer/TokenKind.inl"

        Count,
    };

    /// The enum identifier as text ("KwClass"), for diagnostics/debugging.
    [[nodiscard]] std::string_view TokenName ( TokenKind Kind );

    /// The fixed spelling ("class", "->") or "" for dynamic-lexeme tokens.
    [[nodiscard]] std::string_view TokenSpelling ( TokenKind Kind );

    /// If Text is a reserved word, its keyword TokenKind; else Identifier.
    [[nodiscard]] TokenKind KeywordLookup ( std::string_view Text );

    [[nodiscard]] constexpr bool IsKeyword ( TokenKind Kind )
    {
        switch ( Kind )
        {
#define VOLT_KEYWORD( Name, Spelling ) case TokenKind::Name:
#include "Volt/Frontend/Lexer/TokenKind.inl"
            return true;
        default:
            return false;
        }
    }

    /// One lexed token. Lexeme text for dynamic tokens is interned; fixed
    /// tokens carry an invalid Lexeme (use TokenSpelling instead).
    struct Token
    {

        TokenKind Kind = TokenKind::Error;
        Core::SourceRange Range;
        Core::Symbol Lexeme;            // interned text for dynamic tokens
        bool bHasInterpolation = false; // StringLiteral only

        [[nodiscard]] bool Is ( TokenKind Other ) const
        {
            return Kind == Other;
        }
    };

} // namespace Frontend

} // namespace Volt
