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
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

    // The text the REPL prelude writes before the value itself.
    //
    // `__volt_repl_echo` (source/Volt/REPL/Prelude/Repl.vl) prints the arrow
    // and then `value.inspect`, both straight to descriptor 1 and both from
    // inside the JIT — which is what keeps the two halves in the order they
    // are read, on a descriptor no C++ stream is buffering.
    //
    // Named here because a terminal front end has to tell one half from the
    // other in order to colour them differently, and the alternative — the
    // host printing the arrow itself — puts a flush between two writers on one
    // descriptor and makes their order a matter of discipline.
    inline constexpr std::string_view EchoPrefix = "=> ";

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
        // job (the REPL prelude's `__volt_repl_echo`), and it writes to a
        // descriptor rather than to a string. Only the type is the host's to
        // say.
        std::string ResultType;

        // The session name holding this line's value, when the line produced
        // one that can render itself. Empty for a `def`, for an assignment,
        // and for a value whose type answers no `inspect`.
        //
        // Handed back rather than echoed here so the *front end* decides when
        // the value is written, and therefore what happens around it: a pipe
        // wants it on the descriptor untouched, and a terminal wants it
        // captured and re-coloured. Neither decision belongs in a module that
        // is not allowed to write anything (`Echo`).
        std::string ResultBinding;
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

        // Render the value an outcome bound, by calling the REPL prelude's
        // `__volt_repl_echo` on it. The text lands on descriptor 1, written by
        // Volt code — this returns only whether the call ran.
        //
        // Separate from Feed so the caller owns the moment: it can print an
        // arrow first, or redirect the descriptor and colour what comes back.
        [[nodiscard]] bool Echo ( std::string_view Binding );

        // How many lines have been fed in, whatever became of them. What
        // labels the next one.
        [[nodiscard]] std::size_t LineCount () const;

        // Throw the session away and start a fresh one with the same options.
        // Everything goes: the units, the type store, the JIT and every
        // variable declared at the prompt.
        [[nodiscard]] bool Reset ( std::string &OutError );

        // How many JIT generations are still resident.
        //
        // The observable half of the ephemeral-generation rule: it does not
        // move across a `:type`, and it comes back to where it started across
        // a `:bench`. A leak here is a number that grew, rather than memory
        // nobody ever notices.
        [[nodiscard]] std::size_t LiveGenerations () const;

        // --- Session facts --------------------------------------------------
        //
        // What the `:` builtins and the completer are built out of. Every one
        // of them returns data: a type is a string, a layout is a list of
        // fields, a disassembly is text. Nothing here formats anything, and
        // nothing here writes anything.

        struct TypeAnswer
        {

            bool bOk = false;
            std::string Name;
            // The compiler's own report, when the expression did not compile.
            std::string Diagnostics;
        };

        // The type of an expression, without evaluating it.
        //
        // The expression is compiled — there is no other way to know its type —
        // and the emission is then abandoned: no generation is opened, no
        // module reaches the JIT, and nothing runs. Asking a question about a
        // line must not have the side effects of running it.
        [[nodiscard]] TypeAnswer TypeOf ( std::string_view Expression );

        struct FieldFact
        {

            std::string Name;
            std::string Type;
            std::size_t Offset = 0;
            std::size_t Size   = 0;
            std::size_t Align  = 1;
        };

        struct LayoutAnswer
        {

            bool bOk = false;
            std::string Type;
            std::string Kind; // "Primitive", "Pointer", "Aggregate"
            std::size_t Size  = 0;
            std::size_t Align = 1;
            std::vector<FieldFact> Fields;
            std::string Message;
        };

        // How a type is laid out in memory. Takes a type name, or — failing
        // that — an expression, whose type's layout is what is described.
        [[nodiscard]] LayoutAnswer LayoutOf ( std::string_view Name );

        struct TextAnswer
        {

            bool bOk = false;
            std::string Text;
            std::string Message;
        };

        // The intermediate representation of the line most recently evaluated.
        [[nodiscard]] TextAnswer LastIr () const;

        // The intermediate representation an expression *would* compile to,
        // through the same abandoned emission `TypeOf` takes.
        [[nodiscard]] TextAnswer IrOf ( std::string_view Expression );

        // The machine code a function materialised as. Accepts a Volt name
        // (`twice`, `Array.push`) or a linker symbol verbatim.
        [[nodiscard]] TextAnswer AsmOf ( std::string_view Name, std::size_t MaxBytes = 512 );

        // The source text a declaration was written as, and the block comment
        // written above it. Both take the same names `AsmOf` does.
        [[nodiscard]] TextAnswer SourceOf ( std::string_view Name );
        [[nodiscard]] TextAnswer DocOf ( std::string_view Name );

        struct BenchAnswer
        {

            bool bOk                 = false;
            std::size_t Iterations   = 0;
            std::uint64_t TotalNanos = 0;
            std::uint64_t BestNanos  = 0;
            std::string Message;
            std::string Diagnostics;
        };

        // Run an expression `Iterations` times and time it. The generation it
        // runs in is dropped before this returns.
        [[nodiscard]] BenchAnswer Bench ( std::string_view Expression, std::size_t Iterations );

        struct VariableFact
        {

            std::string Name;
            std::string Type;
        };

        [[nodiscard]] std::vector<VariableFact> Variables () const;

        struct MemberFact
        {

            std::string Name;
            // `( separator : String ) -> Array<String>`, or the field's type.
            std::string Signature;
            std::string Result;
            // The type that actually declares it, when that is not the type
            // the question was asked about.
            std::string Owner;
            bool bMethod = false;
        };

        // Everything reachable through a `.` on an expression: the type's own
        // members, then its mixins', then its superclass's.
        [[nodiscard]] std::vector<MemberFact> MembersOf ( std::string_view Expression );

        // Every member a *named type* declares or inherits, for a completion
        // that has a type name rather than a value.
        [[nodiscard]] std::vector<MemberFact> MembersOfType ( std::string_view TypeName ) const;

        [[nodiscard]] std::vector<std::string> FunctionNames () const;
        [[nodiscard]] std::vector<std::string> TypeNames () const;

        // Is this spelling a type the session knows? What a highlighter asks
        // before repainting an identifier.
        [[nodiscard]] bool KnowsType ( std::string_view Name ) const;
        [[nodiscard]] bool KnowsFunction ( std::string_view Name ) const;

    private:

        struct State;
        std::unique_ptr<State> Impl;
    };

} // namespace Repl

} // namespace Volt
