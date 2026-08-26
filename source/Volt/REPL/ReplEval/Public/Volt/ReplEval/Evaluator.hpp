#pragma once

// Evaluator.hpp — one REPL session's compile-and-run half.
//
// Owns the Driver::ReplSession and decides what happens to a line's outcome;
// knows nothing about how any of it is presented. That split is the module
// rule for the whole REPL tree: everything under REPL/ is pure except
// ReplTui, and "pure" here means it returns values rather than writing them.
//
// Diagnostics are the one thing that arrives as text, because the diagnostic
// renderer is the compiler's and predates this module. It is rendered into a
// string this class owns, never onto a stream this class chose.

#include "ReplEval_export.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace Volt
{

namespace Repl
{

    struct EvaluatorOptions
    {

        std::uint8_t OptLevel = 0;

        // Bypass or refresh the stdlib frontend cache — the same knobs
        // `build` and `run` take, forwarded verbatim.
        bool bNoCache  = false;
        bool bFresh    = false;
        bool bNoStdlib = false;
        bool bVerbose  = false;
    };

    // What one fed line produced. Deliberately not a bool: a line that does not
    // compile, a line that compiles but raises, and a line that runs cleanly
    // are three outcomes a front end renders three different ways.
    enum class EEvalStatus : std::uint8_t
    {

        Ok = 0,
        // Sema rejected it. `Diagnostics` holds the rendered report and
        // nothing ran.
        DidNotCompile = 1,
        // It compiled, and either raised something nobody rescued or the JIT
        // refused to evaluate it. `Message` says which.
        DidNotRun = 2,
    };

    struct EvalOutcome
    {

        EEvalStatus Status = EEvalStatus::Ok;

        // The compiler's own rendered diagnostics for this line alone —
        // warnings included, and present even when Status is Ok.
        std::string Diagnostics;

        // A host-level explanation, set only for DidNotRun.
        std::string Message;

        // The rendered type of what the line produced, when it produced
        // something — `"Int32"`, `"Array<Int32>"`. Empty for a `def`, an
        // assignment, or any statement that is not one bare expression.
        //
        // The *value* is not here and never will be: rendering it is Volt's
        // job (the REPL prelude's `__volt_repl_echo`), and by the time this
        // struct exists the rendered text has already been written. Only the
        // type is the host's to say.
        std::string ResultType;

        // The value text has already been written to the descriptor by the
        // evaluated code. False with a non-empty ResultType means the value
        // exists but has no `to_string` — there is a type to name and nothing
        // truthful to show.
        bool bRendered = false;
    };

    class REPLEVAL_EXPORT Evaluator
    {

    public:

        Evaluator ();
        ~Evaluator ();

        Evaluator ( const Evaluator & )           = delete;
        Evaluator &operator=( const Evaluator & ) = delete;

        // Compile the stdlib and the REPL prelude and materialise them. False
        // with OutError set when the session cannot start; that is the one
        // failure here that is fatal to a session rather than to a line.
        [[nodiscard]] bool Start ( const EvaluatorOptions &Options, std::string &OutError );

        // Compile one line into the session and run it.
        [[nodiscard]] EvalOutcome Feed ( std::string_view Line );

        // How many lines have been fed in, whatever became of them. What
        // labels the next one.
        [[nodiscard]] std::size_t LineCount () const;

    private:

        struct State;
        std::unique_ptr<State> Impl;
    };

} // namespace Repl

} // namespace Volt
