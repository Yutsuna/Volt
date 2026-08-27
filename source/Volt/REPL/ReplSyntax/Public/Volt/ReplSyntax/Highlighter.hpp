#pragma once

// Highlighter.hpp — text in one of three languages, out as coloured spans.
//
// Volt is tokenized by the compiler's own lexer, which is the whole point: a
// REPL that kept a second, approximate grammar for colouring would drift from
// the language on the day a keyword was added, and would be fooled by exactly
// the inputs a lexer is not — `"this is not the end"`, `# def faux`, and a
// string interpolation.
//
// LLVM IR and machine code get scanners of their own, a hundred lines each,
// because neither has a lexer in this project and neither is worth a
// dependency. They are approximate in the way a syntax highlighter is allowed
// to be: an opcode nobody listed paints as plain text, never as an error.
//
// Nothing here writes anything anywhere. A Document is the output.

#include "ReplSyntax_export.hpp"

#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"

#include <functional>
#include <string_view>

namespace Volt
{

namespace Repl
{

    namespace Syntax
    {

        // What a token kind paints as, before anything semantic is known.
        //
        // Backed by a static table rather than a switch: a kind added to
        // TokenKind.inl with no row here still compiles and simply paints as
        // ordinary text, which is the failure a highlighter should have.
        [[nodiscard]] REPLSYNTAX_EXPORT Doc::EPaletteRole RoleOf ( Frontend::TokenKind Kind );

        // An upgrade hook for identifiers the *session* knows something about.
        //
        // The lexer cannot tell a type name from a variable — that is a
        // question for the type store, which lives two modules away — so the
        // caller that has one answers it. Handed the spelling and the role the
        // lexer implies, it returns the role to paint. Absent, every
        // identifier stays an identifier, which is what an editor with no
        // session behind it should do.
        using SemanticHook = std::function<Doc::EPaletteRole( std::string_view, Doc::EPaletteRole )>;

        // Volt source, however incomplete. A half-typed `def f` colours `def`
        // as a keyword and `f` as a name without waiting for its `end`, and an
        // unterminated string colours as a string — the lexer's diagnostics are
        // collected into a throwaway bag and dropped.
        [[nodiscard]] REPLSYNTAX_EXPORT Doc::Document
        HighlightVolt ( std::string_view Text, const Doc::Palette &Theme, const SemanticHook &Known = {} );

        // The same, for text known to hold no newline — what a line editor
        // repaints on every keystroke.
        [[nodiscard]] REPLSYNTAX_EXPORT Doc::Line
        HighlightVoltLine ( std::string_view Text, const Doc::Palette &Theme, const SemanticHook &Known = {} );

        [[nodiscard]] REPLSYNTAX_EXPORT Doc::Document HighlightIr ( std::string_view Text, const Doc::Palette &Theme );

        [[nodiscard]] REPLSYNTAX_EXPORT Doc::Document HighlightAsm ( std::string_view Text, const Doc::Palette &Theme );

    } // namespace Syntax

} // namespace Repl

} // namespace Volt
