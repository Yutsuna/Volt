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

// Every identifier spelling in a line, by the compiler's own lexer. A name
// inside a string literal or a comment is not one, which is why this does not
// grep.
[[nodiscard]] std::unordered_set<std::string> IdentifiersIn ( std::string_view Text )
{
    Volt::Core::StringInterner Interner;
    Volt::Core::DiagEngine::Bag Bag = Volt::Core::DiagEngine::MakeBag();

    Volt::Frontend::Lexer Scanner( Volt::Core::FileId{}, Text, Interner, Bag );

    std::unordered_set<std::string> Out;
    for ( const Volt::Frontend::Token &Tok : Scanner.Tokenize() )
    {
        if ( Tok.Kind == Volt::Frontend::TokenKind::Identifier )
        {
            Out.emplace( Interner.Resolve( Tok.Lexeme ) );
        }
    }
    return Out;
}

} // namespace

struct Volt::Repl::Evaluator::State
{

    Driver::Driver TheDriver;
    Backend::Jit::JitBackend Jit;

    // Grown by one entry per unit rather than rebuilt: MakeBackendViews walks
    // the circuit graph to order its output and an appended unit declares no
    // edges. The span BackendInput hands the emitter points into this, so it
    // has to outlive every call that takes one.
    std::vector<Backend::UnitView> Views;

    std::size_t Lines = 0;
    bool bStarted     = false;

    // One variable the user has declared at a prompt, and where it lives.
    //
    // `Type` is a SemaTypeId, valid across units because every unit interns
    // into the *build's* TypeUniverse. `Symbol` is the storage the declaring
    // unit minted, and it never moves: that unit's module is resident for the
    // rest of the session.
    struct SessionVar
    {

        std::string Name;
        MiddleEnd::TypeSystem::SemaTypeId Type;
        std::string Symbol;
    };

    std::vector<SessionVar> Vars;
    std::unordered_map<std::string, std::size_t> VarByName;

    [[nodiscard]] Backend::BackendInput Input ()
    {
        return Backend::BackendInput{ .Types           = &TheDriver.MutableLayouts(),
                                      .Units           = Views,
                                      .StdlibUnitCount = static_cast<std::uint32_t>( TheDriver.StdlibUnitCount() ) };
    }

    // Build the type annotation for a session variable straight from the type
    // the declaring unit inferred — no round trip through source text, so a
    // type nobody can spell is carried as faithfully as one anybody can.
    [[nodiscard]] Frontend::TypeId
    TypeNodeFor ( Frontend::AstContext &Ast, MiddleEnd::TypeSystem::SemaTypeId Id, std::uint32_t Depth = 0 )
    {
        using namespace MiddleEnd::TypeSystem;

        const TypeStore &Store       = TheDriver.Layouts();
        const TypeUniverse &Universe = Store.Universe();

        if ( Depth > 16 or not Universe.Has( Id ) )
        {
            return Frontend::TypeId{};
        }

        const SemaType &Value = Universe.Get( Id );
        if ( not Value.Base.IsValid() or Value.Base.Value >= Store.TypeCount() )
        {
            return Frontend::TypeId{};
        }

        Frontend::TypeRef Ref;
        Ref.Path.PushBack( Ast.Strings().Intern( Store.Text( Store.Type( Value.Base ).Name ) ) );
        for ( std::size_t Index = 0; Index < Value.Args.Size(); ++Index )
        {
            const Frontend::TypeId Arg = TypeNodeFor( Ast, Value.Args[Index], Depth + 1 );
            if ( not Arg.IsValid() )
            {
                return Frontend::TypeId{};
            }
            Ref.Generics.PushBack( Arg );
        }
        return Ast.Add( Frontend::TypeNode{ std::move( Ref ) } );
    }

    // Name, in this unit, the storage earlier units declared — as the ordinary
    // Volt declaration that means exactly that:
    //
    //     @[External( "volt", "_V_global_36_x" )]
    //     external x : Int32
    //
    // Written as AST rather than as text so the line the user typed keeps its
    // own line and column numbers, and appended rather than prepended for the
    // same reason. ScopeResolver declares module-level names in a pass of their
    // own before any statement is walked, so position does not matter.
    //
    // Only what the line names: Volt has no `eval` and no dynamic lookup, so a
    // variable whose spelling does not appear cannot be reached from it — and
    // declaring them all would make a line cost one declaration per variable
    // the session has ever held, the one term that grows with session length.
    void NameForeignStorage ( Frontend::AstContext &Ast, const std::unordered_set<std::string> &Mentioned )
    {
        for ( const SessionVar &Var : Vars )
        {
            if ( not Mentioned.contains( Var.Name ) )
            {
                continue;
            }

            const Frontend::TypeId Annotation = TypeNodeFor( Ast, Var.Type );
            if ( not Annotation.IsValid() )
            {
                // A type this cannot render is a variable this line cannot see.
                // Dropping it silently is right: refusing the whole line would
                // punish one that may not even mention it.
                continue;
            }

            Frontend::Annotation Marker;
            Marker.Name = Ast.Strings().Intern( "External" );
            Marker.Args.PushBack(
                Ast.Add( Frontend::ExprNode{ Frontend::StringLiteral{ .Loc = {}, .Value = Ast.Strings().Intern( "volt" ) } } ) );
            Marker.Args.PushBack( Ast.Add(
                Frontend::ExprNode{ Frontend::StringLiteral{ .Loc = {}, .Value = Ast.Strings().Intern( Var.Symbol ) } } ) );
            Ast.TopDecls.push_back( Ast.Add( Frontend::DeclNode{ std::move( Marker ) } ) );

            Frontend::ExternalVar Declaration{ .Loc = {}, .Name = Ast.Strings().Intern( Var.Name ), .DeclType = Annotation };
            Ast.TopDecls.push_back( Ast.Add( Frontend::DeclNode{ std::move( Declaration ) } ) );
        }
    }

    // A line that is one bare expression, rewritten into a named binding.
    //
    // Not wrapped directly in a call to `__volt_repl_echo`, because whether the
    // value renders is the one thing that cannot be known before it compiles:
    // `puts( x )` yields an `IO::StandardStream`, which answers no `to_string`,
    // so a blind wrap would turn every `puts` line into a compile error.
    // Binding the value instead always compiles and leaves its type in the
    // store — where the type *is* knowable, and where the decision belongs.
    //
    // Returns the name it bound to, or empty when nothing was rewritten.
    [[nodiscard]] std::string BindResult ( Frontend::AstContext &Ast, std::size_t Serial )
    {
        if ( not Ast.TopDecls.empty() or Ast.TopStmts.size() != 1 )
        {
            return {};
        }

        const Frontend::StmtId Only = Ast.TopStmts[0];
        const auto *Statement       = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Only ) );
        if ( Statement == nullptr or not Statement->Expr.IsValid() )
        {
            return {};
        }

        const Frontend::ExprId Value = Statement->Expr;

        // An assignment is already a binding and already has a name; rewriting
        // it would bind its value twice.
        if ( std::holds_alternative<Frontend::Assign>( Ast.Expr( Value ) ) )
        {
            return {};
        }

        // `counter += 10 if false` is a guard, not a value. An `If` with no
        // `else` yields nothing down the path nobody took, and the checker
        // still types it from the branch that exists — so binding it compiles
        // and echoes whatever that storage happened to hold. Refused by shape,
        // because the type is no help here and the run is too late.
        if ( const auto *Branch = std::get_if<Frontend::If>( &Ast.Expr( Value ) );
             Branch != nullptr and Branch->Else.Size() == 0 )
        {
            return {};
        }

        const std::string Name = "__volt_repl_" + std::to_string( Serial );

        // No type annotation: the checker infers it from the initialiser, the
        // way `buf = expr` is inferred, and what it infers is what is read back.
        Frontend::LocalDecl Bound{ .Loc      = Frontend::LocOf( Ast.Expr( Value ) ),
                                   .Name     = Ast.Strings().Intern( Name ),
                                   .DeclType = Frontend::TypeId{},
                                   .Init     = Value };

        Ast.Stmt( Only ) = Frontend::StmtNode{ std::move( Bound ) };
        return Name;
    }

    // What this unit declared that the session did not already have. Read off
    // the unit's root scope rather than its AST, so an implicit `x = 5` — which
    // has no LocalDecl at all — is picked up the way an annotated one is.
    void Harvest ( std::uint32_t Ordinal )
    {
        const Driver::CompileUnit &Unit = TheDriver.Unit( Ordinal );
        if ( Unit.Scopes.Size() == 0 )
        {
            return;
        }

        const MiddleEnd::Resolver::ScopeId Root{ 0 };
        const MiddleEnd::Resolver::Scope &Top = Unit.Scopes.Get( Root );
        if ( Top.Kind != MiddleEnd::Resolver::EScopeKind::Unit )
        {
            return;
        }

        for ( const auto &[Name, Binding] : Top.Bindings )
        {
            std::string Text( Unit.Ast.Text( Name ) );
            if ( VarByName.contains( Text ) )
            {
                continue; // already ours; this unit only named it again
            }

            const MiddleEnd::TypeSystem::SemaTypeId Type = Unit.Types.SiteType( Binding.Site );
            if ( not Type.IsValid() )
            {
                continue;
            }

            VarByName.emplace( Text, Vars.size() );
            Vars.push_back(
                SessionVar{ .Name = Text, .Type = Type, .Symbol = "_V_global_" + std::to_string( Ordinal ) + "_" + Text } );
        }
    }

    [[nodiscard]] bool IsPrintable ( MiddleEnd::TypeSystem::SemaTypeId Id ) const
    {
        using namespace MiddleEnd::TypeSystem;

        const TypeStore &Store = TheDriver.Layouts();
        if ( not Store.Universe().Has( Id ) )
        {
            return false;
        }

        const SemaType &Value = Store.Universe().Get( Id );
        return Value.Base.IsValid() and Store.LookupMember( Value.Base, "inspect" ).Decl != nullptr;
    }

    [[nodiscard]] std::string DescribeType ( MiddleEnd::TypeSystem::SemaTypeId Id ) const
    {
        const MiddleEnd::TypeSystem::TypeStore &Store = TheDriver.Layouts();
        return Store.Universe().Describe( Store, Id );
    }

    [[nodiscard]] EvalOutcome Feed ( std::string_view Line, bool bMayEcho );
};

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
        return EvalOutcome{ .Status      = EEvalStatus::DidNotRun,
                            .Diagnostics = {},
                            .Message     = "repl: the session was never started",
                            .ResultType  = {},
                            .bRendered   = false };
    }
    return Impl->Feed( Line, /*bMayEcho=*/true );
}

Volt::Repl::EvalOutcome Volt::Repl::Evaluator::State::Feed ( const std::string_view Line, const bool bMayEcho )
{
    const std::size_t Serial = Lines;
    const std::string Label  = "<repl:" + std::to_string( Serial + 1 ) + ">";

    const std::unordered_set<std::string> Mentioned = IdentifiersIn( Line );

    const std::size_t Index = TheDriver.AppendUnit( Label, std::string( Line ) );
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

    const Driver::Driver::UnitResult Unit = TheDriver.AnalyzeUnit( Index );

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
        if ( not Bound.empty() )
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

    // A unit of its own, and the smallest one a session compiles: one call
    // through the prelude's `__volt_repl_echo`. bMayEcho is false so it is not
    // itself treated as a bare expression and bound and echoed in turn.
    const EvalOutcome Echoed = Feed( "__volt_repl_echo( " + Bound + " )\n", /*bMayEcho=*/false );
    Outcome.bRendered        = Echoed.Status == EEvalStatus::Ok;
    return Outcome;
}

std::size_t Volt::Repl::Evaluator::LineCount () const
{
    return Impl->Lines;
}
