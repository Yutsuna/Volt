#pragma once

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/SourceLocation.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Frontend
{

    /// Hand-written, single-pass lexer. Newlines are significant (Volt is a
    /// newline-terminated language) and emitted as Newline tokens; runs of
    /// blank lines collapse into one. String interpolation is left intact in
    /// the lexeme and expanded later by the parser.
    class Lexer
    {

    public:

        Lexer ( Core::FileId File, std::string_view Source, Core::StringInterner &Interner, Core::DiagEngine::Bag &Diagnostics );

        /// Produce the next token (Eof repeats once the end is reached).
        [[nodiscard]] Token Next ();

        /// Convenience: run to completion, returning every token incl. Eof.
        [[nodiscard]] std::vector<Token> Tokenize ();

    private:

        [[nodiscard]] bool AtEnd () const
        {
            return Pos >= Source.size();
        }

        [[nodiscard]] char Peek ( std::size_t Ahead = 0 ) const
        {
            const std::size_t Index = Pos + Ahead;
            return Index < Source.size() ? Source[Index] : '\0';
        }

        char Advance ()
        {
            return Source[Pos++];
        }

        [[nodiscard]] std::uint32_t Offset ( std::size_t Index ) const
        {
            return static_cast<std::uint32_t>( Index );
        }

        [[nodiscard]] Core::SourceRange RangeFrom ( std::size_t Start ) const
        {
            return Core::SourceRange{ File, Offset( Start ), Offset( Pos ) };
        }

        [[nodiscard]] Token Make ( TokenKind Kind, std::size_t Start ) const;
        [[nodiscard]] Token MakeText ( TokenKind Kind, std::size_t Start );

        void SkipInlineWhitespace ();
        bool SkipCommentOrDoc (); // returns true if it consumed something

        [[nodiscard]] Token LexNewline ( std::size_t Start );
        [[nodiscard]] Token LexIdentifier ( std::size_t Start );
        [[nodiscard]] Token LexNumber ( std::size_t Start );
        [[nodiscard]] Token LexString ( std::size_t Start );
        [[nodiscard]] Token LexChar ( std::size_t Start );
        [[nodiscard]] Token LexSymbolOrColon ( std::size_t Start );
        [[nodiscard]] Token LexPunct ( std::size_t Start );

        Core::FileId File;
        std::string_view Source;
        Core::StringInterner &Interner;
        Core::DiagEngine::Bag &Diagnostics;
        std::size_t Pos = 0;
    };

} // namespace Frontend

} // namespace Volt
