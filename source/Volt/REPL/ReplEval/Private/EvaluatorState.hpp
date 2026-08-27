#pragma once

// EvaluatorState.hpp — one session's private state, shared by the two halves
// of this module.
//
// Split out of Evaluator.cpp for a reason and not for length: the *session
// facts* (SessionFacts.cpp) are queries over exactly this state — the Driver's
// type store, the table of variables declared at a prompt, the JIT the session
// runs in — and they are a different kind of code from the incremental
// compile-and-run loop. Both need the state; neither needs the other.
//
// Private, and stays private: nothing outside this module sees a Driver, a
// SemaTypeId or a JitBackend through it.

#include "Volt/ReplEval/Evaluator.hpp"

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendJIT/JitBackend.hpp"
#include "Volt/Driver/Driver.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeUniverse.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Volt
{

namespace Repl
{

    // Every identifier spelling in a line, by the compiler's own lexer. A name
    // inside a string literal or a comment is not one, which is why this does
    // not grep. Shared by the two halves of this module: the compile loop uses
    // it to decide which session variables a line can see, and the completer
    // to decide what a half-typed word might be.
    [[nodiscard]] std::unordered_set<std::string> IdentifiersIn ( std::string_view Text );

} // namespace Repl

} // namespace Volt

struct Volt::Repl::Evaluator::State
{

    Driver::Driver TheDriver;
    Backend::Jit::JitBackend Jit;

    // Grown by one entry per unit rather than rebuilt: MakeBackendViews walks
    // the circuit graph to order its output and an appended unit declares no
    // edges. The span BackendInput hands the emitter points into this, so it
    // has to outlive every call that takes one.
    std::vector<Backend::UnitView> Views;

    EvaluatorOptions Options;
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

    // --- Asking a question about a line, without running it ------------------

    // One unit compiled far enough to be *asked* something, and no further.
    //
    // The unit stays in the Driver and keeps its ordinal — an analysis cannot
    // be rolled back, which the module already lives with for a line that
    // fails sema — but it is never pushed onto Views and never emitted by
    // anything but the caller, and only for as long as the caller wants it.
    struct Scratch
    {

        bool bOk              = false;
        std::size_t Index     = 0;
        std::uint32_t Ordinal = 0;
        // The name a bare expression was bound to, when one was asked for.
        std::string Bound;
        std::string Diagnostics;
    };

    [[nodiscard]] Scratch Analyze ( std::string_view Text, bool bBind );

    // The type a scratch unit's binding came out as.
    [[nodiscard]] MiddleEnd::TypeSystem::SemaTypeId TypeOfBinding ( const Scratch &Unit ) const;

    // Run `Body` with `Unit`'s view appended to Views, and take it back off
    // afterwards however `Body` ends. A probe and a benchmark both need the
    // unit visible to the emitter for exactly one call, and neither may leave
    // it in a span every later line is emitted against.
    template <typename Fn> auto WithScratchView ( const Scratch &Unit, Fn &&Body )
    {
        struct Guard
        {

            std::vector<Backend::UnitView> &Where;

            ~Guard ()
            {
                Where.pop_back();
            }
        };

        Views.push_back( TheDriver.ViewOf( Unit.Ordinal ) );
        const Guard Restore{ Views };
        return Body( Views.back() );
    }

    // --- Naming a declaration -------------------------------------------------

    // What `:src twice`, `:doc Array.map` and `:asm String.size` all resolve
    // through: a spelling, split on `.` or `#`, into the declaration it names.
    struct Found
    {

        bool bOk = false;
        MiddleEnd::TypeSystem::NominalId Owner; // invalid for a free function
        const MiddleEnd::TypeSystem::Member *Entry = nullptr;
        // Set instead of Entry when the name is a type rather than a member.
        bool bType = false;
        std::string Message;
    };

    [[nodiscard]] Found Resolve ( std::string_view Name ) const;

    // The source range a declaration was written over, or an invalid range.
    [[nodiscard]] Core::SourceRange RangeOf ( std::uint32_t Unit, Frontend::DeclId Decl ) const;

    // Why a declaration this session knows about has no text to show.
    //
    // Almost always one answer: the stdlib came out of the frontend cache,
    // which holds published *interfaces* and not ASTs, so a member resolved
    // through it has a signature and no body anywhere in this process. Saying
    // that is worth a sentence — it is a property of how the session started,
    // and one flag away from not being true.
    [[nodiscard]] std::string WhyNoSource ( std::uint32_t Unit ) const;

    // A signature type as text, in the owner's own parameter space.
    [[nodiscard]] std::string
    DescribeSig ( MiddleEnd::TypeSystem::SigTypeId Id, MiddleEnd::TypeSystem::NominalId Owner, std::uint32_t Depth = 0 ) const;

    // One store member as the fact a completer or a query renders.
    [[nodiscard]] MemberFact FactOf ( const MiddleEnd::TypeSystem::Member &Entry,
                                      MiddleEnd::TypeSystem::NominalId Owner,
                                      MiddleEnd::TypeSystem::NominalId AskedAbout ) const;

    // Every member reachable on a nominal: its own, then its mixins', then its
    // superclass's — the same order LookupMember searches in, so what a
    // completion offers is what a call would actually reach.
    void CollectMembers ( MiddleEnd::TypeSystem::NominalId Id,
                          MiddleEnd::TypeSystem::NominalId AskedAbout,
                          std::vector<MemberFact> &Out,
                          std::unordered_set<std::string> &Seen,
                          std::uint32_t Depth = 0 ) const;
};
