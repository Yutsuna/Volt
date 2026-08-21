// DriverRepl.cpp — ReplSession: `volt repl`, one unit at a time.
//
// The third consumer of the same three-phase protocol DriverBuild.cpp and
// DriverRun.cpp use, and the only one that never finishes. Start() compiles the
// stdlib plus the REPL prelude and materialises them exactly as a run would;
// after that every line is one more unit appended to a Driver that is still
// alive, emitted alone against everything already resident.
//
// What makes that cheap is stated once, here, because it is the whole design:
//
//   - the front end compiles the new unit only (Driver::CompileOneMore), with
//     every earlier unit handed to the cross-unit passes as a null AST;
//   - the emission declares everything and defines only the new unit
//     (IrOptions::SkipUnitsBelow), so ORC codegens one small module;
//   - a unit-scope binding is already a module global under PerUnit, so a
//     variable typed on line 2 has a fixed address line 530 can name.
//
// Everything backend-specific stays inside this TU's VOLT_ENABLE_JIT branch —
// Driver.hpp mentions no backend type, so the REPL front end never acquires an
// LLVM dependency.

#include "Volt/Driver/Driver.hpp"

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#ifdef VOLT_ENABLE_JIT
    #include "Volt/BackendCore/TargetBackend.hpp"
    #include "Volt/BackendJIT/JitBackend.hpp"
#endif

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// Where Repl.vl lives, resolved the same way source/Lib is: an explicit
// override, then an installed tree beside the executable, then the build-time
// source path. Kept beside ResolveStdlibDir's shape rather than sharing it,
// because the two answer for different trees and only one of them is hashed
// into the frontend cache key.
[[nodiscard]] fs::path ResolveReplPrelude ()
{
    if ( const char *Override = std::getenv( "VOLT_REPL_PRELUDE" ); Override != nullptr and *Override != '\0' )
    {
        return { Override };
    }

#if defined( VOLT_DEV_REPL_PRELUDE_DIR )
    return fs::path{ VOLT_DEV_REPL_PRELUDE_DIR } / "Repl.vl";
#else
    return "source/Volt/REPL/Prelude/Repl.vl";
#endif
}

// Every identifier spelling in a line, by the compiler's own lexer. A name
// inside a string literal or a comment is not one, which is exactly why this
// does not grep.
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

#ifndef VOLT_ENABLE_JIT

struct Volt::Driver::ReplSession::State
{
};

Volt::Driver::ReplSession::ReplSession () : Impl( std::make_unique<State>() )
{
}

Volt::Driver::ReplSession::~ReplSession () = default;

bool Volt::Driver::ReplSession::Start ( const ReplSessionOptions &Options, std::ostream &Out, std::string &OutError )
{
    static_cast<void>( Options );
    static_cast<void>( Out );
    OutError = "This build of volt was configured without the JIT (enable_jit=false); `volt repl` is unavailable";
    return false;
}

Volt::Driver::ReplSession::LineResult Volt::Driver::ReplSession::Eval ( std::string Label, std::string Text, std::ostream &Out )
{
    static_cast<void>( Label );
    static_cast<void>( Text );
    static_cast<void>( Out );
    return LineResult{ .bCompiled = false, .bRan = false, .Message = "repl: this build has no JIT" };
}

std::size_t Volt::Driver::ReplSession::LineCount () const
{
    return 0;
}

#else

struct Volt::Driver::ReplSession::State
{

    Driver TheDriver;
    Backend::Jit::JitBackend Jit;

    // Grown by one entry per line rather than rebuilt: MakeBackendViews walks
    // the circuit graph to order its output, and a REPL adds no edges — so
    // appending is both cheaper and exactly as correct.
    //
    // The span BackendInput hands the emitter points into this, so it has to
    // outlive every call that takes one.
    std::vector<Backend::UnitView> Views;

    std::size_t Lines = 0;
    bool bStarted     = false;

    // One variable the user has declared at a prompt, and where it lives.
    //
    // `Type` is a SemaTypeId, which is valid across lines because every unit
    // interns into the *build's* TypeUniverse (Driver.cpp, CompileRefs: "`G<A>`
    // is one handle no matter which file wrote it"). `Symbol` is the storage
    // the declaring line minted, and it never moves — that line's module is
    // resident for the rest of the session.
    struct SessionVar
    {

        std::string Name;
        MiddleEnd::TypeSystem::SemaTypeId Type;
        std::string Symbol;
    };

    std::vector<SessionVar> Vars;
    std::unordered_map<std::string, std::size_t> VarByName;

    // Per line, the binding sites its synthetic declarations occupy, mapped to
    // the storage they stand for. Held in a deque because a UnitView points
    // into it and the views outlive any one line.
    std::deque<std::unordered_map<MiddleEnd::Resolver::BindingSite, std::string, MiddleEnd::Resolver::BindingSiteHash>> Maps;

    // Build the type annotation for a session variable, straight from the type
    // the declaring line inferred — no round trip through source text, so a
    // type nobody can spell (an anonymous instantiation) is carried as
    // faithfully as one anybody can.
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

    // Everything earlier lines declared, as declarations this line owns.
    //
    // A `LocalDecl` with no initialiser: it opens no storage of its own — the
    // emitter is told the storage already exists (UnitView::ExternalGlobals) —
    // it only gives the resolver a binding and the checker a type, by the
    // ordinary path, with no special case anywhere in sema.
    void Seed ( Frontend::AstContext &Ast,
                const std::unordered_set<std::string> &Mentioned,
                std::unordered_map<std::string, Frontend::StmtId> &OutSites )
    {
        for ( const SessionVar &Var : Vars )
        {
            // Only what this line names. Volt has no `eval` and no dynamic
            // lookup, so a variable whose spelling does not appear in the text
            // cannot be reached from it — and seeding it anyway would make a
            // line cost one declaration per variable the session has ever
            // held, which is the one term that grows with session length.
            if ( not Mentioned.contains( Var.Name ) )
            {
                continue;
            }

            const Frontend::TypeId Annotation = TypeNodeFor( Ast, Var.Type );
            if ( not Annotation.IsValid() )
            {
                // A type this cannot render is a variable this line cannot see.
                // Dropping it silently is right: the alternative is refusing a
                // line that may not even mention it.
                continue;
            }

            Frontend::LocalDecl Decl{ .Loc          = {},
                                      .Name         = Ast.Strings().Intern( Var.Name ),
                                      .DeclType     = Annotation,
                                      .Init         = Frontend::ExprId{},
                                      .bAlreadyLive = true };
            const Frontend::StmtId Id = Ast.Add( Frontend::StmtNode{ std::move( Decl ) } );

            // At the front, so a binding exists before any statement that reads
            // it — ScopeResolver walks TopStmts in order.
            Ast.TopStmts.insert( Ast.TopStmts.begin(), Id );
            OutSites.emplace( Var.Name, Id );
        }
    }

    // What this line declared that the session did not already have.
    //
    // Read off the unit's own root scope rather than off the AST, so an
    // implicit `x = 5` — which has no LocalDecl at all — is picked up the same
    // way an annotated one is.
    void Harvest ( std::uint32_t Ordinal )
    {
        const CompileUnit &Unit = TheDriver.Unit( Ordinal );
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
                continue; // already ours; this line only re-declared it
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

    [[nodiscard]] Backend::BackendInput Input ()
    {
        return Backend::BackendInput{ .Types           = &TheDriver.MutableLayouts(),
                                      .Units           = Views,
                                      .StdlibUnitCount = static_cast<std::uint32_t>( TheDriver.StdlibUnitCount() ) };
    }
};

Volt::Driver::ReplSession::ReplSession () : Impl( std::make_unique<State>() )
{
}

Volt::Driver::ReplSession::~ReplSession () = default;

bool Volt::Driver::ReplSession::Start ( const ReplSessionOptions &Options, std::ostream &Out, std::string &OutError )
{
    const fs::path Prelude = ResolveReplPrelude();

    std::error_code Ec;
    if ( not fs::is_regular_file( Prelude, Ec ) )
    {
        OutError = "repl: the REPL prelude was not found at '" + Prelude.string() + "' (set VOLT_REPL_PRELUDE to point at it)";
        return false;
    }

    const CompileResult Compiled = Impl->TheDriver.CompileFiles( { Prelude.string() }, Options.CacheOpts );
    if ( Compiled.Errors != 0 )
    {
        Impl->TheDriver.RenderDiagnostics( Out );
        OutError = "repl: the REPL prelude does not compile";
        return false;
    }

    // Indirect linkage is not optional here, unlike in a run: redefining a
    // function at the prompt means repointing its slot, and a call emitted as a
    // direct relocation has no slot to repoint.
    Backend::Jit::JitOptions JitOpts;
    JitOpts.OptLevel         = Options.OptLevel;
    JitOpts.bPerUnitModules  = true;
    JitOpts.bIndirectLinkage = true;

    // The precompiled stdlib, for the same reason `volt run` loads it: it
    // dominates materialisation, and a session pays that cost before the first
    // prompt is even drawn.
    BuildOptions ArtifactOpts;
    ArtifactOpts.StdlibArtifactKind = "shared";
    ArtifactOpts.OptLevel           = Options.OptLevel;
    ArtifactOpts.bVerbose           = Options.bVerbose;

    if ( const std::optional<std::string> Artifact = EnsureStdlibArtifact( Impl->TheDriver, ArtifactOpts ) )
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

Volt::Driver::ReplSession::LineResult Volt::Driver::ReplSession::Eval ( std::string Label, std::string Text, std::ostream &Out )
{
    if ( not Impl->bStarted )
    {
        return LineResult{ .bCompiled = false, .bRan = false, .Message = "repl: the session was never started" };
    }

    // Where the synthetic declarations this line gets will land. Filled by the
    // seed below, read right after to point them at the storage they stand for.
    std::unordered_map<std::string, Frontend::StmtId> Seeded;

    // Which session variables this line could possibly reach: the identifiers
    // it actually spells. Lexed with the compiler's own lexer so a name inside
    // a string or a comment does not count — the same reason ReplCore::Classify
    // reads tokens rather than text.
    const std::unordered_set<std::string> Mentioned = IdentifiersIn( Text );

    const Driver::EvalLineResult Line =
        Impl->TheDriver.EvalLine( std::move( Label ), std::move( Text ), [this, &Seeded, &Mentioned] ( Frontend::AstContext &Ast )
                                  { Impl->Seed( Ast, Mentioned, Seeded ); } );
    ++Impl->Lines;

    // Rendered and dropped in one step, whatever the outcome: the engine
    // outlives the session, and a line's warnings are as much this line's as
    // its errors are.
    Impl->TheDriver.ConsumeLineDiagnostics( Line.DiagMark, Out );

    if ( not Line.bOk )
    {
        // The unit stays in the Driver and keeps its ordinal. Rolling back a
        // half-published interface is not something the seam can do, and
        // leaving a declared-but-bodyless member behind costs a store entry
        // nothing will ever emit. It is never evaluated, which is what matters.
        return LineResult{ .bCompiled = false, .bRan = false, .Message = {} };
    }

    // The map has to outlive the view that points at it, and a view outlives
    // the line — hence the deque rather than a local.
    auto &Foreign = Impl->Maps.emplace_back();
    for ( const auto &[Name, Site] : Seeded )
    {
        const auto Known = Impl->VarByName.find( Name );
        if ( Known != Impl->VarByName.end() )
        {
            Foreign.emplace( MiddleEnd::Resolver::BindingSite{ Site }, Impl->Vars[Known->second].Symbol );
        }
    }

    Backend::UnitView View = Impl->TheDriver.ViewOf( Line.Ordinal );
    View.ExternalGlobals   = &Foreign;
    Impl->Views.push_back( View );

    // Before running it, not after: the line's own declarations become part of
    // the session whether or not its statements complete, exactly as they would
    // in a program that raised halfway through its initialiser.
    Impl->Harvest( Line.Ordinal );

    const Backend::BackendInput Build = Impl->Input();
    const Backend::RunResult Ran      = Impl->Jit.EvalUnit( Build, Impl->Views.back() );
    if ( not Ran.bOk )
    {
        return LineResult{ .bCompiled = true, .bRan = false, .Message = Ran.Message };
    }

    return LineResult{ .bCompiled = true, .bRan = true, .Message = {} };
}

std::size_t Volt::Driver::ReplSession::LineCount () const
{
    return Impl->Lines;
}

#endif
