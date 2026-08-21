// Evaluator.cpp — the session, and the line policy over it.

#include "Volt/ReplEval/Evaluator.hpp"

#include "Volt/Driver/Driver.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <utility>

struct Volt::Repl::Evaluator::State
{

    Driver::ReplSession Session;
};

Volt::Repl::Evaluator::Evaluator () : Impl( std::make_unique<State>() )
{
}

Volt::Repl::Evaluator::~Evaluator () = default;

bool Volt::Repl::Evaluator::Start ( const EvaluatorOptions &Options, std::string &OutError )
{
    Driver::ReplSessionOptions SessionOpts;
    SessionOpts.OptLevel            = Options.OptLevel;
    SessionOpts.CacheOpts.bNoCache  = Options.bNoCache;
    SessionOpts.CacheOpts.bFresh    = Options.bFresh;
    SessionOpts.CacheOpts.bNoStdlib = Options.bNoStdlib;
    SessionOpts.CacheOpts.bVerbose  = Options.bVerbose;
    SessionOpts.bVerbose            = Options.bVerbose;

    // The prelude's own diagnostics are part of the failure to start, not
    // something a front end renders later, so they are folded into OutError.
    std::ostringstream Report;
    if ( Impl->Session.Start( SessionOpts, Report, OutError ) )
    {
        return true;
    }

    if ( const std::string Rendered = Report.str(); not Rendered.empty() )
    {
        OutError = Rendered + OutError;
    }
    return false;
}

Volt::Repl::EvalOutcome Volt::Repl::Evaluator::Feed ( const std::string_view Line )
{
    // The label is what every diagnostic for this line names, and what `:src`
    // will resolve against later. One-based so it matches the prompt.
    const std::string Label = "<repl:" + std::to_string( Impl->Session.LineCount() + 1 ) + ">";

    std::ostringstream Diagnostics;
    const Driver::ReplSession::LineResult Result = Impl->Session.Eval( Label, std::string( Line ), Diagnostics );

    EvalOutcome Outcome;
    Outcome.Diagnostics = Diagnostics.str();

    if ( not Result.bCompiled )
    {
        Outcome.Status = EEvalStatus::DidNotCompile;
        return Outcome;
    }
    if ( not Result.bRan )
    {
        Outcome.Status  = EEvalStatus::DidNotRun;
        Outcome.Message = Result.Message;
        return Outcome;
    }

    Outcome.Status = EEvalStatus::Ok;
    return Outcome;
}

std::size_t Volt::Repl::Evaluator::LineCount () const
{
    return Impl->Session.LineCount();
}
