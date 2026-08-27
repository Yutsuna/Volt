// Evaluator.cpp — one interactive session: a Driver that keeps growing, and the
// JIT the units it compiles are evaluated in.
//
// Everything here is built out of ordinary compiler API. Nothing below this
// module knows a REPL exists, and nothing here asks the compiler to behave
// differently than it would for any other caller:
//
//   - a line is one more unit (Driver::AppendUnit / AnalyzeUnit);
//   - a variable an earlier line declared is named by an `external` declaration
//     this module writes into the new unit's AST, which is the language's own
//     way to say "storage that lives elsewhere";
//   - a value is rendered by Volt code in the REPL prelude, never by this file.
//
// This module owns a BackendJIT directly, unlike Volt::CLI, which owns none.
// That is the point of the layering, not a hole in it: a backend consumer is
// allowed to name a backend, and confining that to here is what keeps the CLI
// free of LLVM.

#include "Volt/ReplEval/Evaluator.hpp"

#include "EvaluatorState.hpp"

#include "Volt/BackendCore/TargetBackend.hpp"
#include "Volt/BackendJIT/JitBackend.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Driver/Driver.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeUniverse.hpp"

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// Where Repl.vl lives, resolved the way source/Lib is: an explicit override,
// then an installed tree beside the executable, then the build-time path.
[[nodiscard]] fs::path ResolvePrelude ()
{
    if ( const char *Override = std::getenv( "VOLT_REPL_PRELUDE" ); Override != nullptr and *Override != '\0' )
    {
        return { Override };
    }

#if defined( VOLT_REPL_PRELUDE_DIR )
    return fs::path{ VOLT_REPL_PRELUDE_DIR } / "Repl.vl";
#else
    return "source/Volt/REPL/Prelude/Repl.vl";
#endif
}

} // namespace

std::unordered_set<std::string> Volt::Repl::IdentifiersIn ( const std::string_view Text )
{
    Volt::Core::StringInterner Interner;
    Volt::Core::DiagEngine::Bag Bag = Volt::Core::DiagEngine::MakeBag();

    Volt::Frontend::Lexer Scanner( Volt::Core::FileId{}, Text, Interner, Bag );

    std::unordered_set<std::string> Out;
    for ( const Volt::Frontend::Token &Tok : Scanner.Tokenize() )
    {
        if ( Tok.Kind == Volt::Frontend::TokenKind::Identifier or Tok.Kind == Volt::Frontend::TokenKind::Constant )
        {
            Out.emplace( Interner.Resolve( Tok.Lexeme ) );
        }
    }
    return Out;
}

Volt::Repl::Evaluator::Evaluator () : Impl( std::make_unique<State>() )
{
}

Volt::Repl::Evaluator::~Evaluator () = default;

bool Volt::Repl::Evaluator::Start ( const EvaluatorOptions &Options, std::string &OutError )
{
    const fs::path Prelude = ResolvePrelude();

    std::error_code Ec;
    if ( not fs::is_regular_file( Prelude, Ec ) )
    {
        OutError = "repl: the prelude was not found at '" + Prelude.string() + "' (set VOLT_REPL_PRELUDE to point at it)";
        return false;
    }

    Impl->Options = Options;

    Driver::FCacheOptions CacheOpts;
    CacheOpts.bNoCache  = Options.bNoCache;
    CacheOpts.bFresh    = Options.bFresh;
    CacheOpts.bNoStdlib = Options.bNoStdlib;
    CacheOpts.bVerbose  = Options.bVerbose;

    if ( Impl->TheDriver.CompileFiles( { Prelude.string() }, CacheOpts ).Errors != 0 )
    {
        std::ostringstream Report;
        Impl->TheDriver.RenderDiagnostics( Report );
        OutError = Report.str() + "repl: the prelude does not compile";
        return false;
    }

    // Indirect linkage is not optional here, unlike in a run: replacing a
    // function means repointing its slot, and a call emitted as a direct
    // relocation has no slot to repoint.
    Backend::Jit::JitOptions JitOpts;
    JitOpts.OptLevel         = Options.OptLevel;
    JitOpts.bPerUnitModules  = true;
    JitOpts.bIndirectLinkage = true;

    // The precompiled stdlib, for the same reason `volt run` loads it: it
    // dominates materialisation, and this pays that cost before the first
    // prompt is drawn.
    Driver::BuildOptions ArtifactOpts;
    ArtifactOpts.StdlibArtifactKind = "shared";
    ArtifactOpts.OptLevel           = Options.OptLevel;
    ArtifactOpts.bVerbose           = Options.bVerbose;

    if ( const std::optional<std::string> Artifact = Driver::EnsureStdlibArtifact( Impl->TheDriver, ArtifactOpts ) )
    {
        JitOpts.Dylibs.insert( JitOpts.Dylibs.begin(), *Artifact );
        JitOpts.SkipUnitsBelow = static_cast<std::uint32_t>( Impl->TheDriver.StdlibUnitCount() );
    }

    Impl->Jit.SetOptions( std::move( JitOpts ) );

    // `:ir` reads what the last line actually compiled to rather than
    // re-emitting it — a second emission of a line whose symbols are already
    // defined produces declarations and nothing else.
    Impl->Jit.RecordIr( true );

    Impl->Views                       = Impl->TheDriver.MakeBackendViews();
    const Backend::BackendInput Build = Impl->Input();

    Impl->Jit.Begin( Build );
    for ( const Backend::UnitView &Unit : Build.Units )
    {
        if ( Impl->Jit.EmitUnit( Unit ) != Backend::EEmitStatus::Ok )
        {
            OutError = "repl: the session's own units did not emit";
            return false;
        }
    }

    const Backend::EmitResult Materialised = Impl->Jit.Finalize();
    if ( Materialised.Status != Backend::EEmitStatus::Ok )
    {
        OutError = "repl: " + Materialised.Message;
        return false;
    }

    // Runs `__volt_entry`, which calls `_V_init_all` — every resident unit's
    // top-level statements, the stdlib's included. Nothing else initialises
    // them, and a session whose stdlib was never initialised fails in ways that
    // look like the user's fault.
    const Backend::RunResult Primed = Impl->Jit.Run( {} );
    if ( not Primed.bOk )
    {
        OutError = "repl: the session did not start: " + Primed.Message;
        return false;
    }

    Impl->bStarted = true;
    return true;
}

Volt::Repl::EvalOutcome Volt::Repl::Evaluator::Feed ( const std::string_view Line )
{
    if ( not Impl->bStarted )
    {
        return EvalOutcome{ .Status        = EEvalStatus::DidNotRun,
                            .Diagnostics   = {},
                            .Message       = "repl: the session was never started",
                            .ResultType    = {},
                            .ResultBinding = {} };
    }
    return Impl->Feed( Line, /*bMayEcho=*/true );
}

Volt::Repl::EvalOutcome Volt::Repl::Evaluator::State::Feed ( const std::string_view Line, const bool bMayEcho )
{
    const std::size_t Serial = Lines;
    const std::string Label  = "<repl:" + std::to_string( Serial + 1 ) + ">";

    const std::unordered_set<std::string> Mentioned = IdentifiersIn( Line );

    const Driver::Driver::AppendedUnit Appended = TheDriver.AppendUnit( Label, std::string( Line ) );
    const std::size_t Index                     = Appended.Index;
    ++Lines;

    // The window between parse and analysis, and the only thing this module
    // does to a unit that a file on disk would not already contain.
    std::string Bound;
    {
        Frontend::AstContext &Ast = TheDriver.UnitAt( Index ).Ast;
        if ( bMayEcho )
        {
            Bound = BindResult( Ast, Serial );
        }
        NameForeignStorage( Ast, Mentioned );
    }

    const Driver::Driver::UnitResult Unit = TheDriver.AnalyzeUnit( Index, Appended.DiagMark );

    EvalOutcome Outcome;
    {
        std::ostringstream Report;
        TheDriver.ConsumeLineDiagnostics( Unit.DiagMark, Report );
        Outcome.Diagnostics = Report.str();
    }

    if ( not Unit.bOk )
    {
        // A bare expression was rewritten into a binding on the guess that it
        // was worth one, and `assert!( x == 5 )` is the guess being wrong: the
        // callee returns `Void`, so the binding — not the line — is what has no
        // type. Nothing ran, because a unit that does not compile never reaches
        // EvalUnit, so feeding it again as the statement the user actually
        // wrote costs a second analysis and no second side effect. The line
        // keeps its number: the first attempt is a unit nobody will hear about,
        // and its diagnostics describe a line nobody typed.
        //
        // Only when it parsed. The rewrite happens *after* the parse, so a
        // line that did not parse will not parse any differently the second
        // time — retrying it would cost a unit and report the same two syntax
        // errors under a different ordinal.
        if ( Appended.bParsed and not Bound.empty() )
        {
            Lines = Serial;
            return Feed( Line, /*bMayEcho=*/false );
        }

        // The unit stays in the Driver and keeps its ordinal. Rolling back a
        // half-published interface is not something the seam can do, and a
        // declared-but-bodyless member costs a store entry nothing will emit.
        // It is never evaluated, which is what matters.
        Outcome.Status = EEvalStatus::DidNotCompile;
        return Outcome;
    }

    Views.push_back( TheDriver.ViewOf( Unit.Ordinal ) );

    // Before running it: what the unit declared belongs to the session whether
    // or not its statements complete, exactly as in a program that raised
    // halfway through an initialiser.
    Harvest( Unit.Ordinal );

    const Backend::BackendInput Build = Input();
    const Backend::RunResult Ran      = Jit.EvalUnit( Build, Views.back() );
    if ( not Ran.bOk )
    {
        Outcome.Status  = EEvalStatus::DidNotRun;
        Outcome.Message = Ran.Message;
        return Outcome;
    }

    Outcome.Status = EEvalStatus::Ok;
    if ( Bound.empty() )
    {
        return Outcome;
    }

    // --- Show what it produced ------------------------------------------------
    const auto Known = VarByName.find( Bound );
    if ( Known == VarByName.end() )
    {
        return Outcome;
    }

    const MiddleEnd::TypeSystem::SemaTypeId Type = Vars[Known->second].Type;
    Outcome.ResultType                           = DescribeType( Type );

    // `_` is the last value, the way it is in every REPL worth using. Another
    // name for the same storage, re-pointed rather than reassigned, so its type
    // follows the last line instead of being fixed by the first.
    const std::string Symbol = Vars[Known->second].Symbol;
    if ( const auto Last = VarByName.find( "_" ); Last != VarByName.end() )
    {
        Vars[Last->second].Type   = Type;
        Vars[Last->second].Symbol = Symbol;
    }
    else
    {
        VarByName.emplace( "_", Vars.size() );
        Vars.push_back( SessionVar{ .Name = "_", .Type = Type, .Symbol = Symbol } );
    }

    if ( not IsPrintable( Type ) )
    {
        // Nothing truthful to print. Volt has no universal `inspect`, so a type
        // that cannot render itself is named and no more.
        return Outcome;
    }

    // Named, not echoed. The call that renders it is one more unit — the
    // smallest one a session ever compiles — and when it runs is the front
    // end's business: a pipe wants the bytes on the descriptor in the order
    // they were produced, and a terminal wants to capture and re-colour them.
    Outcome.ResultBinding = Bound;
    return Outcome;
}

bool Volt::Repl::Evaluator::Echo ( const std::string_view Binding )
{
    if ( not Impl->bStarted or Binding.empty() )
    {
        return false;
    }

    // bMayEcho is false so this call is not itself treated as a bare
    // expression, bound, and echoed in turn.
    //
    // Recording is off for the length of it. This unit is the REPL's own
    // bookkeeping, not a line anybody typed, and `:ir` asked immediately after
    // a result should show the expression that produced it rather than the
    // call that printed it.
    Impl->Jit.RecordIr( false );
    const EvalOutcome Echoed = Impl->Feed( "__volt_repl_echo( " + std::string( Binding ) + " )\n", /*bMayEcho=*/false );
    Impl->Jit.RecordIr( true );

    return Echoed.Status == EEvalStatus::Ok;
}

std::size_t Volt::Repl::Evaluator::LineCount () const
{
    return Impl->Lines;
}
