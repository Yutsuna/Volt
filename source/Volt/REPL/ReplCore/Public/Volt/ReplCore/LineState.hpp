#pragma once

// LineState.hpp — is what the user has typed so far a complete Volt statement?
//
// The question every REPL has to answer before it evaluates anything, and the
// one irb answers with Ripper. Here it is answered with the compiler's own
// lexer, which is the point: a REPL that counted `def` and `end` in raw text
// would be fooled by `"this is not the end"`, by `# def faux`, and by a string
// interpolation — and it would drift from the language every time the grammar
// gained a keyword. The lexer already skips comments, already lexes a string
// whole, and already knows every keyword there is.
//
// Pure, like everything under REPL/ except ReplTui: it reads text and returns
// a verdict.

#include "ReplCore_export.hpp"

#include <cstdint>
#include <string_view>

namespace Volt
{

namespace Repl
{

    enum class ELineState : std::uint8_t
    {

        // Evaluate it.
        Complete = 0,

        // A block is open, a bracket is unclosed, a literal never ended, or the
        // last thing typed was an operator with nothing after it. Ask for
        // another line and lex the whole thing again — a REPL line is short and
        // the lexer is not the cost.
        NeedsMore = 1,
    };

    // The verdict for everything typed so far, newlines included.
    //
    // One-sided in the safe direction: an input this cannot make sense of reads
    // as Complete, so the compiler reports the syntax error rather than the
    // prompt hanging on a line that will never close.
    [[nodiscard]] REPLCORE_EXPORT ELineState Classify ( std::string_view Accumulated );

    // Does this line continue the one before it rather than begin one of its
    // own?
    //
    //     raw_users
    //       .filter( (&.empty?.!) )
    //       .map( &transform )
    //
    // True for a line whose first token cannot start a statement — a leading
    // `.`, `&.`, `|>`, `<|`, `::`, `>>`, `<<` or `,`. The question Classify
    // cannot answer: Classify is handed everything typed so far and this is
    // about what comes next, so only a caller holding the next line can ask
    // it. Blank lines and comment-only lines answer false.
    [[nodiscard]] REPLCORE_EXPORT bool ContinuesPrevious ( std::string_view Line );

} // namespace Repl

} // namespace Volt
