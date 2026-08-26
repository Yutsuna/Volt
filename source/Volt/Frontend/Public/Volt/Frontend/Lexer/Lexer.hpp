#pragma once

#include "Frontend_export.hpp"
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
    class FRONTEND_EXPORT Lexer
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

        [[nodiscard]] static std::uint32_t Offset ( std::size_t Index )
        {
            return static_cast<std::uint32_t>( Index );
        }

        [[nodiscard]] Core::SourceRange RangeFrom ( std::size_t Start ) const
        {
            return Core::SourceRange{ .File = File, .Begin = Offset( Start ), .End = Offset( Pos ) };
        }

        [[nodiscard]] Token Make ( TokenKind Kind, std::size_t Start ) const;
        [[nodiscard]] Token MakeText ( TokenKind Kind, std::size_t Start );

        void SkipInlineWhitespace ();
        bool SkipCommentOrDoc (); // returns true if it consumed something

        [[nodiscard]] Token LexNewline ( std::size_t Start );
        [[nodiscard]] Token LexIdentifier ( std::size_t Start );
        [[nodiscard]] Token LexNumber ( std::size_t Start );
        // `"..."` and `` `...` `` differ only in their delimiter and in what
        // the token means; the scan — escapes, `#{ ... }` interpolation at
        // brace depth — is written once, in LexQuoted.
        [[nodiscard]] Token LexString ( std::size_t Start );
        [[nodiscard]] Token LexCommand ( std::size_t Start );
        [[nodiscard]] Token LexQuoted ( std::size_t Start, char Terminator, TokenKind Kind, std::string_view What );
        [[nodiscard]] Token LexIvarInterp ( std::size_t Start );
        [[nodiscard]] Token LexChar ( std::size_t Start );
        [[nodiscard]] Token LexSymbolOrColon ( std::size_t Start );
        [[nodiscard]] Token LexPunct ( std::size_t Start );

        Core::FileId File;
        std::string_view Source;
        Core::StringInterner &Interner;
        Core::DiagEngine::Bag &Diagnostics;
        std::size_t Pos = 0;

        // Set by SkipCommentOrDoc when a `#{` ran off the end of the input,
        // cleared by the Error token Next hands back in its place.
        bool bUnterminatedDoc = false;
    };

} // namespace Frontend

} // namespace Volt
