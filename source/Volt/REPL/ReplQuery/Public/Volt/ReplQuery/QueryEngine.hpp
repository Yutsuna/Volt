#pragma once

// QueryEngine.hpp — the `:` builtins, as a function from a line to a document.
//
// Every one of them is a question about the session rather than a statement in
// it: `:type` names a type without evaluating anything, `:src` reads text the
// compiler already holds, `:bench` runs an expression in a generation it drops
// before it returns. None of them writes anything — the answer is a Document,
// and whoever asked decides where it goes.
//
// The builtin table is declared once, here, and read by three callers: the
// parser below, `:help`, and the completer. A builtin added to that table is
// therefore parsed, documented and completable with no second edit.

#include "ReplQuery_export.hpp"

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

    namespace Query
    {

        enum class EBuiltin : std::uint8_t
        {

            // Not a builtin at all — an ordinary Volt line, which includes a
            // symbol literal the table below does not name.
            None = 0,

            Help,
            Type,
            Layout,
            Ir,
            Asm,
            Src,
            Doc,
            Bench,
            History,
            Vars,
            Reset,
            Exit,
        };

        struct Builtin
        {

            EBuiltin Kind = EBuiltin::None;
            std::string_view Name;
            // What it takes, for the help table. Empty when it takes nothing.
            std::string_view Argument;
            std::string_view Summary;
            // Does the answer belong in a side panel rather than in the
            // transcript? A disassembly does; a type name does not.
            bool bPanel = false;
        };

        // Every builtin there is, in the order `:help` lists them.
        [[nodiscard]] REPLQUERY_EXPORT std::span<const Builtin> Builtins ();

        struct Command
        {

            EBuiltin Kind = EBuiltin::None;
            std::string Name;
            std::string Argument;
        };

        // What a line means, if it means a builtin.
        //
        // A line is a builtin only when its first word is a colon followed by a
        // name the table above holds. That is what keeps `:pending` a symbol
        // literal — an ordinary Volt expression, which it is — while `:type`
        // is a command. The alternative, treating every leading colon as a
        // command, would make a perfectly good expression unreachable.
        [[nodiscard]] REPLQUERY_EXPORT Command Parse ( std::string_view Line );

        enum class EPlacement : std::uint8_t
        {

            // In the transcript, where the prompt is.
            Inline = 0,
            // In the side panel, or in a pager when the terminal is too narrow
            // to have one.
            Panel,
        };

        struct Result
        {

            bool bOk             = false;
            bool bExit           = false;
            EPlacement Placement = EPlacement::Inline;
            std::string Title;
            Doc::Document Body;
        };

        // Answers builtins over one session.
        //
        // Holds references, not copies: the session it questions outlives it,
        // and the palette it colours with can be swapped by the front end
        // between calls.
        class REPLQUERY_EXPORT Engine
        {

        public:

            Engine ( Evaluator &InSession, const Doc::Palette &InTheme ) : Session( InSession ), Theme( InTheme )
            {
            }

            // How many iterations `:bench` runs when the line does not say.
            // Small on purpose: a REPL answer that takes a second is a REPL
            // answer nobody waits for, and `:bench 100000 expr` is one word
            // away.
            static constexpr std::size_t DefaultBenchIterations = 100;

            // How many bytes `:asm` decodes before giving up, when the
            // function has no return in reach. A REPL line's initialiser is
            // tens of bytes; a stdlib method is hundreds.
            static constexpr std::size_t AsmWindow = 512;

            // How wide the answer may be. Zero — the default — means "as wide
            // as it wants", which is what a pipe gets: a table written to a
            // file has no width to fit into, and squeezing one would put
            // ellipses into a golden.
            //
            // A number, rather than a terminal, because this module has never
            // seen one and is not going to start.
            void SetWidth ( const std::size_t InColumns )
            {
                Columns = InColumns;
            }

            [[nodiscard]] Result Run ( const Command &What, std::span<const std::string> History = {} );

        private:

            [[nodiscard]] Result Help () const;
            [[nodiscard]] Result Type ( std::string_view Expression ) const;
            [[nodiscard]] Result Layout ( std::string_view Name ) const;
            [[nodiscard]] Result Ir ( std::string_view Expression ) const;
            [[nodiscard]] Result Asm ( std::string_view Name ) const;
            [[nodiscard]] Result Src ( std::string_view Name ) const;
            [[nodiscard]] Result DocFor ( std::string_view Name ) const;
            [[nodiscard]] Result Bench ( std::string_view Argument ) const;
            [[nodiscard]] Result History ( std::span<const std::string> Lines ) const;
            [[nodiscard]] Result Vars () const;
            [[nodiscard]] Result Reset () const;

            [[nodiscard]] Result Failure ( std::string Message ) const;

            Evaluator &Session;
            const Doc::Palette &Theme;
            std::size_t Columns = 0;
        };

    } // namespace Query

} // namespace Repl

} // namespace Volt
