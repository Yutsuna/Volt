#pragma once

// Completer.hpp — what the half-typed word at the cursor could become.
//
// Semantic, not lexical: `text.` offers what `text`'s *type* answers, which
// means typing the expression first. That is the whole reason this module sits
// over ReplEval rather than over a word list — a completion built from every
// identifier ever seen would offer `push` on a String, and be wrong in the one
// place a completion is worth having.
//
// Typed, never emitted and never run. Asking what a value's members are must
// not compile machine code, and it certainly must not execute any.
//
// Pure, like everything here except ReplTui: it returns candidates, and
// drawing a popup around them is somebody else's job.

#include "ReplComplete_export.hpp"

#include "Volt/ReplDoc/Document.hpp"
#include "Volt/ReplDoc/Palette.hpp"
#include "Volt/ReplEval/Evaluator.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Repl
{

    namespace Complete
    {

        enum class EKind : std::uint8_t
        {

            Variable = 0,
            Function,
            Type,
            Member,
            Field,
            Builtin,
        };

        struct Candidate
        {

            // What replaces the range below when this one is chosen.
            std::string Text;
            // The signature or type, shown beside it and never inserted.
            std::string Detail;
            EKind Kind = EKind::Variable;
        };

        struct Completion
        {

            // The byte range of the line these candidates replace — the
            // half-typed word, or an empty range at the cursor when nothing
            // has been typed yet.
            std::size_t Begin = 0;
            std::size_t End   = 0;

            std::vector<Candidate> Candidates;

            // The longest prefix every candidate shares. What Tab inserts when
            // the list is still ambiguous, which is how a completion that
            // cannot decide still makes progress.
            std::string CommonPrefix;

            [[nodiscard]] bool Empty () const
            {
                return Candidates.empty();
            }
        };

        class REPLCOMPLETE_EXPORT Completer
        {

        public:

            explicit Completer ( Evaluator &InSession ) : Session( InSession )
            {
            }

            // Everything the word ending at `Cursor` could be.
            //
            // Three shapes, decided by what sits before the word: a `.` means
            // members of whatever is to its left; a leading `:` on an
            // otherwise-untouched line means a builtin; anything else means a
            // name in scope — session variables first, then free functions,
            // then types.
            [[nodiscard]] Completion At ( std::string_view Line, std::size_t Cursor );

            // What the *session* knows about a spelling, for a highlighter
            // that has one.
            //
            // The lexer can tell a Constant from an identifier and no more:
            // whether `Enumerable` names a type and whether `twice` names a
            // function are questions for the type store, which is two modules
            // away from a tokenizer. This is the answer, in the shape
            // `ReplSyntax::SemanticHook` asks for — one hash lookup, so it is
            // cheap enough to run on every keystroke.
            [[nodiscard]] Doc::EPaletteRole Classify ( std::string_view Name, Doc::EPaletteRole Lexical ) const;

            // The rest of the most recent history line that begins with what
            // has been typed. Fish's ghost text, and the same rule: the newest
            // match wins, and a line that is already complete suggests nothing.
            [[nodiscard]] static std::string GhostText ( std::string_view Line, std::span<const std::string> History );

            // The candidate list as a document — one row each, name then
            // detail, the selected row reversed.
            [[nodiscard]] static Doc::Document
            Render ( const Completion &What, const Doc::Palette &Theme, std::size_t Selected, std::size_t MaxRows );

        private:

            Evaluator &Session;
        };

    } // namespace Complete

} // namespace Repl

} // namespace Volt
