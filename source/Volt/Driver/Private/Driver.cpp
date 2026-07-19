#include "Volt/Driver/Driver.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Driver/WellKnown.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstDump.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"
#include "Volt/Sema/Pass.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace Volt
{

namespace Driver
{

    namespace fs = std::filesystem;

    namespace
    {

        [[nodiscard]] bool HasSuffix ( std::string_view Path, std::string_view Suffix )
        {
            return Path.size() >= Suffix.size() && Path.substr( Path.size() - Suffix.size() ) == Suffix;
        }

        [[nodiscard]] bool IsComponentPath ( std::string_view Path )
        {
            return HasSuffix( Path, WellKnown::ComponentExt );
        }

        [[nodiscard]] bool IsSourceFile ( const fs::path &Path )
        {
            const std::string Ext = Path.extension().string();
            return Ext == WellKnown::SourceExt || Ext == WellKnown::ComponentExt;
        }

    } // namespace

    void Driver::ReportDriver ( Core::ESeverity Severity, std::string Message )
    {
        Diagnostics.Report( Core::Diagnostic{ Severity, Core::SourceRange{ DriverFile, 0, 0 }, std::move( Message ), {} } );
    }

    bool Driver::Load ( const std::string &Path, Core::FileId &OutFile, std::string &OutText )
    {
        std::ifstream Stream( Path, std::ios::binary );
        if ( !Stream )
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

    void Driver::ParseOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
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

    void Driver::RunSemaOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
    {
        // Passes (JsxLowering included) run per file over local state; the
        // published Registry is the only shared input, and it is read-only.
        Sema::PassContext Context{ Unit.Ast, Unit.Types, Bag, Unit.Stats, &Registry };
        static_cast<void>( Sema::RunPasses( Context ) );
    }

    void Driver::LowerOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
    {
        // Lowerings rewrite purely local state; no published interfaces.
        Sema::PassContext Context{ Unit.Ast, Unit.Types, Bag, Unit.Stats };
        static_cast<void>( Sema::RunPasses( Context, Sema::EPassKind::Lowering ) );
    }

    CompileResult Driver::CompileRefs ( const std::vector<SourceRef> &Refs, EPipeline Pipeline )
    {
        // Phase 1 (serial): register every file's text and unit up front so
        // the parallel phases only touch per-unit state + the diag engine.
        for ( const SourceRef &Ref : Refs )
        {
            Core::FileId File;
            std::string Text;
            if ( !Load( Ref.Path, File, Text ) )
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
            const std::size_t Count    = Units.size();
            const std::size_t Hardware = std::max<std::size_t>( 1, std::thread::hardware_concurrency() );
            const std::size_t Workers  = std::min( Hardware, std::max<std::size_t>( 1, Count ) );

            std::atomic<std::size_t> Next{ 0 };

            std::vector<std::jthread> Pool;
            Pool.reserve( Workers );
            for ( std::size_t W = 0; W < Workers; ++W )
            {
                Pool.emplace_back(
                    [&]
                    {
                        Core::DiagEngine::Bag Bag = Diagnostics.MakeBag();
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

        // Phase 2 (parallel): lex + parse every unit into its own arenas.
        ForEachUnitParallel( &Driver::ParseOne );

        if ( Pipeline == EPipeline::Full )
        {
            // Phase 3 (serial): publish each unit's top-level interface. This is
            // the cross-unit seam — after this point the Registry is frozen and
            // sema may read any unit's exported declarations without locks.
            for ( std::size_t Index = 0; Index < Units.size(); ++Index )
            {
                static_cast<void>(
                    Sema::PublishUnitInterface( Units[Index].Ast, static_cast<std::uint32_t>( Index ), Registry ) );
            }

            // Phase 4 (parallel): run the sema passes over every parsed unit.
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
            Result.JsxLowered += Unit.Stats.JsxLowered;
        }
        return Result;
    }

    CompileResult Driver::CompileFiles ( const std::vector<std::string> &Paths )
    {
        std::vector<SourceRef> Refs;
        Refs.reserve( Paths.size() );
        for ( const std::string &Path : Paths )
        {
            Refs.push_back( SourceRef{ Path, std::string{}, IsComponentPath( Path ) } );
        }
        return CompileRefs( Refs );
    }

    CompileResult Driver::ParseFiles ( const std::vector<std::string> &Paths, bool bLowered )
    {
        std::vector<SourceRef> Refs;
        Refs.reserve( Paths.size() );
        for ( const std::string &Path : Paths )
        {
            Refs.push_back( SourceRef{ Path, std::string{}, IsComponentPath( Path ) } );
        }
        return CompileRefs( Refs, bLowered ? EPipeline::ParseAndLower : EPipeline::ParseOnly );
    }

    void Driver::BuildLinkGraph ( const std::string &RootModule )
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
                if ( Annotation == nullptr || Unit.Ast.Text( Annotation->Name ) != WellKnown::LinkAnnotation )
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

    CompileResult Driver::CompileCircuit ( const std::string &ProjectPath )
    {
        // The manifest is its own unit; parse it to read entrypoint + modules.
        Core::FileId ProjectFile;
        std::string ProjectText;
        if ( !Load( ProjectPath, ProjectFile, ProjectText ) )
        {
            CompileResult Failed;
            Failed.Errors = Diagnostics.ErrorTotal();
            return Failed;
        }

        Core::StringInterner ManifestInterner;
        Frontend::AstContext Manifest{ ManifestInterner, ProjectFile };
        {
            Core::DiagEngine::Bag Bag = Diagnostics.MakeBag();
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

                if ( Name == WellKnown::EntrypointKey && Call->Args.Size() > 0 )
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
                        if ( Key && Dir )
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
        if ( !EntryRel.empty() )
        {
            const fs::path Entry = ProjectDir / EntryRel;
            Refs.push_back( SourceRef{ Entry.string(), CircuitName, IsComponentPath( EntryRel ) } );
        }

        for ( const auto &[ModName, RelDir] : Modules )
        {
            const fs::path Dir = ProjectDir / RelDir;
            std::error_code Ec;
            if ( !fs::is_directory( Dir, Ec ) )
            {
                ReportDriver( Core::ESeverity::Warning, "module '" + ModName + "' directory not found: " + Dir.string() );
                Circuit.AddModule( ModName );
                continue;
            }
            for ( const fs::directory_entry &It : fs::recursive_directory_iterator( Dir, Ec ) )
            {
                if ( It.is_regular_file() && IsSourceFile( It.path() ) )
                {
                    Refs.push_back( SourceRef{ It.path().string(), ModName, IsComponentPath( It.path().string() ) } );
                }
            }
        }

        CompileResult Result = CompileRefs( Refs );
        BuildLinkGraph( CircuitName );

        Result.Errors = Diagnostics.ErrorTotal();
        Result.bCycle = !Circuit.FindCycle().empty();
        return Result;
    }

    void Driver::DumpUnits ( std::ostream &Out, const Frontend::FAstDumpOptions &Options ) const
    {
        for ( const CompileUnit &Unit : Units )
        {
            Frontend::AstDumper Dumper( Unit.Ast, Sources, Out, Options );
            Dumper.DumpFile();
        }
    }

} // namespace Driver

} // namespace Volt
