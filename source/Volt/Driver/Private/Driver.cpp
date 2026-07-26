#include "Volt/Driver/Driver.hpp"

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/Diagnostic.hpp"
#include "Volt/Driver/WellKnown.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstDump.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"
#include "Volt/Sema/Pass.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{

[[nodiscard]] bool HasSuffix ( std::string_view Path, std::string_view Suffix )
{
    return Path.size() >= Suffix.size() and Path.substr( Path.size() - Suffix.size() ) == Suffix;
}

// Where the running `volt` binary itself lives, so stdlib discovery does not
// depend on the caller's CWD. Linux-only (`/proc/self/exe`) since that is the
// only platform this toolchain currently targets (rules/cpp-style.md).
[[nodiscard]] fs::path ExecutableDir ()
{
    std::error_code Ec;
    const fs::path Exe = fs::read_symlink( "/proc/self/exe", Ec );
    if ( Ec )
    {
        return {};
    }
    return Exe.parent_path();
}

// Resolves `source/Lib` independent of CWD, in priority order:
//   1. `VOLT_STDLIB_DIR` env override — explicit escape hatch for dev/test.
//   2. `<exe_dir>/../share/volt/Lib` — the conventional install layout, so
//      this keeps working once a real `install()` rule ships (no cost today).
//   3. The checked-out `source/Lib` baked in at configure time
//      (`VOLT_DEV_STDLIB_DIR`, see Driver/CMakeLists.txt) — guarantees a
//      dev build finds the real stdlib regardless of the invocation's CWD.
[[nodiscard]] fs::path ResolveStdlibDir ()
{
    if ( const char *Override = std::getenv( "VOLT_STDLIB_DIR" ); Override != nullptr and *Override != '\0' )
    {
        return { Override };
    }

    std::error_code Ec;
    if ( const fs::path ExeDir = ExecutableDir(); not ExeDir.empty() )
    {
        if ( const fs::path Installed = ExeDir / ".." / "share" / "volt" / "Lib"; fs::is_directory( Installed, Ec ) )
        {
            return Installed;
        }
    }

#if defined( VOLT_DEV_STDLIB_DIR )
    return { VOLT_DEV_STDLIB_DIR };
#else
    return "source/Lib";
#endif
}

[[nodiscard]] bool IsComponentPath ( std::string_view Path )
{
    return HasSuffix( Path, Volt::Driver::WellKnown::ComponentExt );
}

[[nodiscard]] bool IsSourceFile ( const fs::path &Path )
{
    const std::string Ext = Path.extension().string();
    return Ext == Volt::Driver::WellKnown::SourceExt or Ext == Volt::Driver::WellKnown::ComponentExt;
}

} // namespace

std::optional<fs::path> Volt::Driver::DiscoverManifest ( const fs::path &InPath )
{
    std::error_code Ec;
    fs::path Dir = fs::absolute( fs::is_directory( InPath, Ec ) ? InPath : InPath.parent_path(), Ec );

    while ( !Dir.empty() )
    {
        fs::path Candidate = Dir / WellKnown::ManifestName;
        if ( fs::is_regular_file( Candidate, Ec ) )
        {
            return Candidate;
        }
        const fs::path Parent = Dir.parent_path();
        if ( Parent == Dir )
        {
            break;
        }
        Dir = Parent;
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
void Volt::Driver::Driver::ReportDriver ( Core::ESeverity Severity, std::string Message )
{
    Diagnostics.Report( Core::Diagnostic{ .Severity = Severity,
                                          .Range    = Core::SourceRange{ .File = DriverFile, .Begin = 0, .End = 0 },
                                          .Message  = std::move( Message ),
                                          .Notes    = {} } );
}

bool Volt::Driver::Driver::Load ( const std::string &Path, Core::FileId &OutFile, std::string &OutText )
{
    std::ifstream Stream( Path, std::ios::binary );
    if ( not Stream )
    {
        ReportDriver( Core::ESeverity::Error, "cannot read '" + Path + "'" );
        return false;
    }

    std::ostringstream Buffer;
    Buffer << Stream.rdbuf();
    OutText = Buffer.str();
    OutFile = Sources.AddFile( Path, OutText );
    return true;
}

void Volt::Driver::Driver::ParseOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
{
    const std::string_view Text = Sources.TextOf( Unit.File );

    Frontend::Lexer Lexer( Unit.File, Text, Unit.Interner, Bag );
    std::vector<Frontend::Token> Tokens = Lexer.Tokenize();

    Frontend::Parser Parser( std::move( Tokens ), Unit.Ast, Bag, Text );
    if ( Unit.bComponent )
    {
        Parser.ParseComponentFile();
    }
    else
    {
        Parser.ParseFile();
    }
}

void Volt::Driver::Driver::RunSemaOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
{
    // Passes (JsxLowering included) run per file over local state; the
    // published Registry is the only shared input, and it is read-only.
    Sema::PassContext Context{ .Ast     = Unit.Ast,
                               .Types   = Types,
                               .Values  = Unit.Types,
                               .Scopes  = Unit.Scopes,
                               .Diags   = Bag,
                               .Stats   = Unit.Stats,
                               .Globals = &Registry,
                               .Sources = &Sources,
                               .Callees = &Unit.Callees };
    static_cast<void>( Sema::RunPasses( Context ) );
}

void Volt::Driver::Driver::LowerOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
{
    // Lowerings rewrite purely local state; no published interfaces.
    Sema::PassContext Context{ .Ast     = Unit.Ast,
                               .Types   = Types,
                               .Values  = Unit.Types,
                               .Scopes  = Unit.Scopes,
                               .Diags   = Bag,
                               .Stats   = Unit.Stats,
                               .Sources = &Sources };
    static_cast<void>( Sema::RunPasses( Context, Sema::EPassKind::Lowering ) );
}

Volt::Driver::CompileResult Volt::Driver::Driver::CompileRefs ( const std::vector<SourceRef> &Refs, EPipeline Pipeline )
{
    // register every file's text and unit up front so
    // the parallel phases only touch per-unit state + the diag engine.
    for ( const SourceRef &Ref : Refs )
    {
        Core::FileId File;
        std::string Text;
        if ( not Load( Ref.Path, File, Text ) )
        {
            continue;
        }
        Units.emplace_back( File, Ref.Path, Ref.Module, Ref.bComponent );
    }

    // Workers pull unit indices from a shared atomic and accumulate into
    // a thread-local Bag, merged once at the end (the only lock on the
    // hot path).
    const auto ForEachUnitParallel = [&] ( auto Step )
    {
        const std::size_t Count    = this->Units.size();
        const std::size_t Hardware = std::max<std::size_t>( 1, std::thread::hardware_concurrency() );
        const std::size_t Workers  = std::min( Hardware, std::max<std::size_t>( 1, Count ) );

        std::atomic<std::size_t> Next{ 0 };

        std::vector<std::jthread> Pool;
        Pool.reserve( Workers );
        for ( std::size_t WorkIdx = 0; WorkIdx < Workers; ++WorkIdx )
        {
            Pool.emplace_back(
                [&]
                {
                    Core::DiagEngine::Bag Bag = Core::DiagEngine::MakeBag();
                    for ( ;; )
                    {
                        const std::size_t Index = Next.fetch_add( 1, std::memory_order_relaxed );
                        if ( Index >= Count )
                        {
                            break;
                        }
                        ( this->*Step )( Units[Index], Bag );
                    }
                    Diagnostics.Merge( std::move( Bag ) );
                } );
        }
    }; // jthreads join at the lambda's end

    // parallel: lex + parse every unit into its own arenas.
    ForEachUnitParallel( &Driver::ParseOne );

    if ( Pipeline == EPipeline::Full )
    {
        // serial: publish each unit's top-level interface. This is
        // the cross-unit seam — after this point the Registry is frozen and
        // sema may read any unit's exported declarations without locks.
        Core::DiagEngine::Bag SeamBag = Core::DiagEngine::MakeBag();
        for ( std::size_t Index = 0; Index < Units.size(); ++Index )
        {
            const auto Ordinal = static_cast<std::uint32_t>( Index );
            static_cast<void>( Sema::PublishUnitInterface( Units[Index].Ast, Ordinal, Registry ) );
            // Same seam, same reason: type binding is cross-unit, so it
            // cannot be a per-file parallel pass. Once this loop ends the
            // store is frozen and sema reads it without a lock.
            static_cast<void>( Sema::BindUnitTypes( Units[Index].Ast, Ordinal, Types, SeamBag ) );
        }

        // Every unit's Phase A is done, so every type this build declares
        // exists; attach the structural layout of every non-@[Primitive] one
        // now, before any signature is resolved. Indexed the same way
        // Member::Unit / NominalType::Unit are — discovery order — so this
        // is the array ResolveStructLayouts recurses across when a field
        // names an aggregate declared in a different file.
        std::vector<const Frontend::AstContext *> UnitAsts;
        UnitAsts.reserve( Units.size() );
        for ( const CompileUnit &Unit : Units )
        {
            UnitAsts.push_back( &Unit.Ast );
        }
        Sema::ResolveStructLayouts( UnitAsts, Types );

        // Still serial, still the same seam, but a second pass: a signature
        // may name a type declared in a file that comes later, so every name
        // must exist before any signature is resolved — otherwise stdlib file
        // order would silently decide what a member returns.
        for ( std::size_t Index = 0; Index < Units.size(); ++Index )
        {
            Sema::ResolveUnitSignatures( Units[Index].Ast, static_cast<std::uint32_t>( Index ), Types, SeamBag );
        }
        Diagnostics.Merge( std::move( SeamBag ) );

        // parallel: run the sema passes over every parsed unit.
        ForEachUnitParallel( &Driver::RunSemaOne );
    }
    else if ( Pipeline == EPipeline::ParseAndLower )
    {
        // Lowered parse (`volt parse --lowered`): rewrite each unit's AST
        // in place, still without the cross-unit seam.
        ForEachUnitParallel( &Driver::LowerOne );
    }

    CompileResult Result;
    Result.Files  = Units.size();
    Result.Errors = Diagnostics.ErrorTotal();
    for ( const CompileUnit &Unit : Units )
    {
        Result.Stats.Merge( Unit.Stats );
    }
    return Result;
}

void Volt::Driver::Driver::LoadStdLib ( std::vector<SourceRef> &Refs )
{
    const fs::path LibDir = ResolveStdlibDir();
    std::error_code Ec;
    if ( not fs::is_directory( LibDir, Ec ) )
    {
        return;
    }

    const std::size_t Start = Refs.size();
    for ( const fs::directory_entry &It : fs::recursive_directory_iterator( LibDir, Ec ) )
    {
        if ( It.is_regular_file() and IsSourceFile( It.path() ) )
        {
            Refs.push_back(
                SourceRef{ .Path = It.path().string(), .Module = "Core", .bComponent = IsComponentPath( It.path().string() ) } );
        }
    }

    // `recursive_directory_iterator` follows readdir() order, which is
    // filesystem-dependent, not alphabetical. TypeBinder's Phase A binds each
    // file's layout in the same pass it declares the type (TypeBinder.cpp,
    // `BindType`), so a field naming a type from a file visited later in this
    // walk (e.g. `String#data : Pointer<UInt8>` when "Pointer.vl" has not
    // been bound yet) silently resolves to an invalid LayoutId — a bug no
    // existing test caught, since TypeChecker never reads Layout and only
    // codegen (BackendLLVM) does. A stable, deterministic order is the fix:
    // sorted by path, `Primitives/Pointer.vl` binds before
    // `Primitives/String.vl` on every filesystem.
    std::sort( Refs.begin() + static_cast<std::ptrdiff_t>( Start ), Refs.end(),
               [] ( const SourceRef &A, const SourceRef &B ) { return A.Path < B.Path; } );
}

Volt::Driver::CompileResult Volt::Driver::Driver::CompileFiles ( const std::vector<std::string> &Paths )
{
    std::vector<SourceRef> Refs;
    LoadStdLib( Refs );
    Refs.reserve( Refs.size() + Paths.size() );
    for ( const std::string &Path : Paths )
    {
        Refs.push_back( SourceRef{ .Path = Path, .Module = std::string{}, .bComponent = IsComponentPath( Path ) } );
    }
    return CompileRefs( Refs );
}

Volt::Driver::CompileResult Volt::Driver::Driver::ParseFiles ( const std::vector<std::string> &Paths, bool bLowered )
{
    std::vector<SourceRef> Refs;
    Refs.reserve( Paths.size() );
    for ( const std::string &Path : Paths )
    {
        Refs.push_back( SourceRef{ .Path = Path, .Module = std::string{}, .bComponent = IsComponentPath( Path ) } );
    }
    return CompileRefs( Refs, bLowered ? EPipeline::ParseAndLower : EPipeline::ParseOnly );
}

void Volt::Driver::Driver::BuildLinkGraph ( const std::string &RootModule )
{
    Circuit.AddModule( RootModule );

    // Every top-level `@[Link("Target")]` annotation is an edge from the
    // file's owning module to Target.
    for ( const CompileUnit &Unit : Units )
    {
        const std::string &From = Unit.Module.empty() ? RootModule : Unit.Module;
        Circuit.AddModule( From );

        for ( const Frontend::DeclId Id : Unit.Ast.TopDecls )
        {
            const auto *Annotation = std::get_if<Frontend::Annotation>( &Unit.Ast.Decl( Id ) );
            if ( Annotation == nullptr or Unit.Ast.Text( Annotation->Name ) != WellKnown::LinkAnnotation )
            {
                continue;
            }
            if ( Annotation->Args.Size() == 0 )
            {
                continue;
            }
            if ( const std::optional<std::string_view> Target = Frontend::AsStringText( Unit.Ast, Annotation->Args[0] ) )
            {
                Circuit.AddLink( From, std::string{ *Target } );
            }
        }
    }

    if ( const std::vector<CircuitGraph::NodeIndex> Cycle = Circuit.FindCycle(); !Cycle.empty() )
    {
        std::string Chain;
        for ( std::size_t I = 0; I < Cycle.size(); ++I )
        {
            Chain += Circuit.NameOf( Cycle[I] );
            if ( I + 1 < Cycle.size() )
            {
                Chain += " -> ";
            }
        }
        ReportDriver( Core::ESeverity::Error, "circuit has a dependency cycle: " + Chain );
    }
}

Volt::Driver::CompileResult Volt::Driver::Driver::CompileCircuit ( const std::string &ProjectPath )
{
    // The manifest is its own unit; parse it to read entrypoint + modules.
    Core::FileId ProjectFile;
    std::string ProjectText;
    if ( not Load( ProjectPath, ProjectFile, ProjectText ) )
    {
        CompileResult Failed;
        Failed.Errors = Diagnostics.ErrorTotal();
        return Failed;
    }

    Core::StringInterner ManifestInterner;
    Frontend::AstContext Manifest{ ManifestInterner, ProjectFile };
    {
        Core::DiagEngine::Bag Bag = Core::DiagEngine::MakeBag();
        Frontend::Lexer Lexer( ProjectFile, ProjectText, ManifestInterner, Bag );
        Frontend::Parser Parser( Lexer.Tokenize(), Manifest, Bag, ProjectText );
        Parser.ParseFile();
        Diagnostics.Merge( std::move( Bag ) );
    }

    const fs::path ProjectDir = fs::path( ProjectPath ).parent_path();

    std::string CircuitName{ WellKnown::DefaultCircuitName };
    std::string EntryRel;
    std::vector<std::pair<std::string, std::string>> Modules; // name -> rel dir

    // Walk the circuit manifest: `entrypoint "..."` and `modules(a=>b,...)`.
    // Each recognised key is one clause over the AstQuery helpers — adding
    // a manifest key = a WellKnown constant + one clause here.
    for ( const Frontend::DeclId TopId : Manifest.TopDecls )
    {
        const auto *Circ = std::get_if<Frontend::Circuit>( &Manifest.Decl( TopId ) );
        if ( Circ == nullptr )
        {
            continue;
        }
        CircuitName = std::string{ Manifest.Text( Circ->Name ) };

        for ( const Frontend::StmtId StmtId : Circ->Body )
        {
            const Frontend::Call *Call = Frontend::StmtAsCall( Manifest, StmtId );
            if ( Call == nullptr )
            {
                continue;
            }
            const std::optional<std::string_view> Name = Frontend::CalleeName( Manifest, *Call );

            if ( Name == WellKnown::EntrypointKey and Call->Args.Size() > 0 )
            {
                if ( const std::optional<std::string_view> Ep = Frontend::AsStringText( Manifest, Call->Args[0] ) )
                {
                    EntryRel = std::string{ *Ep };
                }
            }
            else if ( Name == WellKnown::ModulesKey )
            {
                for ( const Frontend::ExprId ArgId : Call->Args )
                {
                    const Frontend::Binary *Pair = Frontend::AsBinaryOp( Manifest, ArgId, Frontend::TokenKind::FatArrow );
                    if ( Pair == nullptr )
                    {
                        continue;
                    }
                    const std::optional<std::string_view> Key = Frontend::AsStringText( Manifest, Pair->Lhs );
                    const std::optional<std::string_view> Dir = Frontend::AsStringText( Manifest, Pair->Rhs );
                    if ( Key and Dir )
                    {
                        Modules.emplace_back( *Key, *Dir );
                    }
                }
            }
        }
    }

    // Gather sources: the entrypoint (owned by the root module) plus every
    // `.vl`/`.vlx` under each declared module directory.
    std::vector<SourceRef> Refs;
    if ( not EntryRel.empty() )
    {
        const fs::path Entry = ProjectDir / EntryRel;
        Refs.push_back( SourceRef{ .Path = Entry.string(), .Module = CircuitName, .bComponent = IsComponentPath( EntryRel ) } );
    }

    for ( const auto &[ModName, RelDir] : Modules )
    {
        const fs::path Dir = ProjectDir / RelDir;
        std::error_code Ec;
        if ( not fs::is_directory( Dir, Ec ) )
        {
            ReportDriver( Core::ESeverity::Warning, "module '" + ModName + "' directory not found: " + Dir.string() );
            Circuit.AddModule( ModName );
            continue;
        }
        for ( const fs::directory_entry &It : fs::recursive_directory_iterator( Dir, Ec ) )
        {
            if ( It.is_regular_file() and IsSourceFile( It.path() ) )
            {
                Refs.push_back( SourceRef{
                    .Path = It.path().string(), .Module = ModName, .bComponent = IsComponentPath( It.path().string() ) } );
            }
        }
    }

    // Same seam as CompileFiles: without the stdlib nothing claims IntLiteral,
    // so every literal in a circuit typed as nothing and the whole tree came
    // out untyped. A circuit is not a different language.
    LoadStdLib( Refs );

    CompileResult Result = CompileRefs( Refs );
    BuildLinkGraph( CircuitName );

    Result.Errors = Diagnostics.ErrorTotal();
    Result.bCycle = !Circuit.FindCycle().empty();
    return Result;
}

void Volt::Driver::Driver::DumpUnits ( std::ostream &Out, const Frontend::FAstDumpOptions &Options ) const
{
    for ( const CompileUnit &Unit : Units )
    {
        Frontend::AstDumper Dumper( Unit.Ast, Sources, Out, Options );
        Dumper.DumpFile();
    }
}

std::vector<Volt::Backend::UnitView> Volt::Driver::Driver::MakeBackendViews () const
{
    std::vector<Backend::UnitView> Views;
    Views.reserve( Units.size() );

    std::vector<bool> bEmitted( Units.size(), false );

    const auto Append = [&] ( std::size_t Index )
    {
        const CompileUnit &Source = Units[Index];
        // The ordinal is the *discovery* index — the very one BindUnitTypes
        // stamped on every Member and NominalType — not the position in this
        // reordered vector.
        Views.push_back( Backend::UnitView{ .Ordinal = static_cast<std::uint32_t>( Index ),
                                            .Module  = Source.Module,
                                            .Path    = Source.Path,
                                            .Ast     = &Source.Ast,
                                            .Values  = &Source.Types,
                                            .Callees = &Source.Callees,
                                            .Scopes  = &Source.Scopes } );
        bEmitted[Index] = true;
    };

    // Dependencies first. A module usually spans several files, so every unit
    // claiming that module name goes out together, in discovery order.
    if ( std::vector<CircuitGraph::NodeIndex> Order; Circuit.TopoOrder( Order ) )
    {
        for ( const CircuitGraph::NodeIndex Node : Order )
        {
            const std::string &Name = Circuit.NameOf( Node );
            for ( std::size_t Index = 0; Index < Units.size(); ++Index )
            {
                if ( not bEmitted[Index] and Units[Index].Module == Name )
                {
                    Append( Index );
                }
            }
        }
    }

    // Whatever the graph did not name: a flat `CompileFiles` build declares no
    // `@[Link]` edge at all, and the stdlib units are pulled in outside the
    // circuit. Discovery order already puts source/Lib/ first.
    for ( std::size_t Index = 0; Index < Units.size(); ++Index )
    {
        if ( not bEmitted[Index] )
        {
            Append( Index );
        }
    }

    return Views;
}
