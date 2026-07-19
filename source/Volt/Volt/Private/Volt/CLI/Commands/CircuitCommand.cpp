#include "Volt/CLI/Commands/CircuitCommand.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/CLI/Version.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Driver/WellKnown.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/**
 * Private helpers
 */

namespace
{

namespace fs = std::filesystem;
using Volt::Core::FLogger;
namespace WellKnown = Volt::Driver::WellKnown;

using FModuleMap = std::vector<std::pair<std::string, std::string>>; //<< name -> rel dir, insertion-ordered

/// The structural content of an existing manifest, read without validating
/// paths against disk — fixing broken paths is exactly this command's job.
struct FManifest
{

    std::string Name;
    std::string Runtime;
    std::string Entrypoint;
    FModuleMap Modules;
};

[[nodiscard]] std::string Quote ( std::string_view Text )
{
    std::string Out;
    Out.reserve( Text.size() + 2 );
    Out += '"';
    for ( const char Char : Text )
    {
        if ( Char == '\\' or Char == '"' )
        {
            Out += '\\';
        }
        Out += Char;
    }
    Out += '"';
    return Out;
}

[[nodiscard]] bool IsSourceFile ( const fs::path &Path )
{
    const std::string Ext = Path.extension().string();
    return Ext == WellKnown::SourceExt or Ext == WellKnown::ComponentExt;
}

/// First-level directories under `Source/` containing at least one source
/// file (recursively), keyed by directory basename, sorted.
[[nodiscard]] FModuleMap ScanModules ( const fs::path &Root )
{
    FModuleMap Modules;
    const fs::path SourceDir = Root / WellKnown::SourceDirName;

    std::error_code Ec;
    if ( !fs::is_directory( SourceDir, Ec ) )
    {
        return Modules;
    }

    std::vector<fs::path> Entries;
    for ( const fs::directory_entry &Entry : fs::directory_iterator( SourceDir, Ec ) )
    {
        if ( Entry.is_directory() )
        {
            Entries.push_back( Entry.path() );
        }
    }
    std::ranges::sort( Entries );

    for ( const fs::path &Dir : Entries )
    {
        bool bHasSource = false;
        for ( const fs::directory_entry &File : fs::recursive_directory_iterator( Dir, Ec ) )
        {
            if ( File.is_regular_file() and IsSourceFile( File.path() ) )
            {
                bHasSource = true;
                break;
            }
        }
        if ( bHasSource )
        {
            Modules.emplace_back( Dir.filename().string(),
                                  ( fs::path( WellKnown::SourceDirName ) / Dir.filename() ).generic_string() );
        }
    }
    return Modules;
}

[[nodiscard]] std::string FormatManifest ( const FManifest &Manifest )
{
    std::string Out;
    Out += "circuit " + Quote( Manifest.Name ) + "\n";
    Out += "{\n";
    Out += "  " + std::string( WellKnown::RuntimeKey ) + " " + Quote( Manifest.Runtime ) + "\n";
    Out += "  " + std::string( WellKnown::EntrypointKey ) + " " + Quote( Manifest.Entrypoint ) + "\n";

    if ( !Manifest.Modules.empty() )
    {
        std::size_t Width = 0;
        for ( const auto &[Name, Dir] : Manifest.Modules )
        {
            Width = std::max( Width, Quote( Name ).size() );
        }

        Out += "\n  " + std::string( WellKnown::ModulesKey ) + "(\n";
        for ( const auto &[Name, Dir] : Manifest.Modules )
        {
            const std::string Key = Quote( Name );
            Out += "    " + Key + std::string( Width - Key.size(), ' ' ) + " => " + Quote( Dir ) + ",\n";
        }
        Out += "  )\n";
    }

    Out += "}\n";
    return Out;
}

/// Structural read of an existing `Project.vl`: one circuit block required,
/// runtime + entrypoint required, module mapping optional.
[[nodiscard]] std::optional<FManifest> ReadManifest ( const fs::path &ManifestPath )
{
    std::ifstream Stream( ManifestPath, std::ios::binary );
    if ( !Stream )
    {
        FLogger::Error( "Cannot read '" + ManifestPath.string() + "'", "circuit" );
        return std::nullopt;
    }
    std::ostringstream Buffer;
    Buffer << Stream.rdbuf();
    const std::string Text = Buffer.str();

    Volt::Core::SourceManager Sources;
    Volt::Core::DiagEngine Diagnostics;
    Volt::Core::StringInterner Interner;

    const Volt::Core::FileId File = Sources.AddFile( ManifestPath.string(), Text );
    Volt::Frontend::AstContext Ast{ Interner, File };
    {
        Volt::Core::DiagEngine::Bag Bag = Volt::Core::DiagEngine::MakeBag();
        Volt::Frontend::Lexer Lexer( File, Text, Interner, Bag );
        Volt::Frontend::Parser Parser( Lexer.Tokenize(), Ast, Bag, Text );
        Parser.ParseFile();
        Diagnostics.Merge( std::move( Bag ) );
    }
    if ( Diagnostics.HasErrors() )
    {
        FLogger::Flush();
        Diagnostics.Render( Sources, std::cerr );
        FLogger::Error( "Failed to load existing " + std::string( WellKnown::ManifestName ), "circuit" );
        return std::nullopt;
    }

    const Volt::Frontend::Circuit *Block = nullptr;
    for ( const Volt::Frontend::DeclId Id : Ast.TopDecls )
    {
        if ( const auto *Circ = std::get_if<Volt::Frontend::Circuit>( &Ast.Decl( Id ) ) )
        {
            if ( Block != nullptr )
            {
                FLogger::Error( "Multiple `circuit` blocks found in " + ManifestPath.string(), "circuit" );
                return std::nullopt;
            }
            Block = Circ;
        }
    }
    if ( Block == nullptr )
    {
        FLogger::Error( "No `circuit` block found in " + ManifestPath.string(), "circuit" );
        return std::nullopt;
    }

    FManifest Manifest;
    Manifest.Name = std::string( Ast.Text( Block->Name ) );

    for ( const Volt::Frontend::StmtId StmtId : Block->Body )
    {
        const Volt::Frontend::Call *Call = Volt::Frontend::StmtAsCall( Ast, StmtId );
        if ( Call == nullptr )
        {
            continue;
        }
        const std::optional<std::string_view> Key = Volt::Frontend::CalleeName( Ast, *Call );

        if ( Key == WellKnown::RuntimeKey and Call->Args.Size() > 0 )
        {
            if ( const auto Value = Volt::Frontend::AsStringText( Ast, Call->Args[0] ) )
            {
                Manifest.Runtime = std::string( *Value );
            }
        }
        else if ( Key == WellKnown::EntrypointKey and Call->Args.Size() > 0 )
        {
            if ( const auto Value = Volt::Frontend::AsStringText( Ast, Call->Args[0] ) )
            {
                Manifest.Entrypoint = std::string( *Value );
            }
        }
        else if ( Key == WellKnown::ModulesKey )
        {
            for ( const Volt::Frontend::ExprId ArgId : Call->Args )
            {
                const Volt::Frontend::Binary *Pair =
                    Volt::Frontend::AsBinaryOp( Ast, ArgId, Volt::Frontend::TokenKind::FatArrow );
                if ( Pair == nullptr )
                {
                    continue;
                }
                const auto Name = Volt::Frontend::AsStringText( Ast, Pair->Lhs );
                const auto Dir  = Volt::Frontend::AsStringText( Ast, Pair->Rhs );
                if ( Name and Dir )
                {
                    Manifest.Modules.emplace_back( *Name, *Dir );
                }
            }
        }
    }

    if ( Manifest.Runtime.empty() )
    {
        FLogger::Error( "Missing `runtime` entry in " + ManifestPath.string(), "circuit" );
        return std::nullopt;
    }
    if ( Manifest.Entrypoint.empty() )
    {
        FLogger::Error( "Missing `entrypoint` entry in " + ManifestPath.string(), "circuit" );
        return std::nullopt;
    }
    return Manifest;
}

/// Fresh manifest: conventional entrypoint + scanned module directories.
[[nodiscard]] std::optional<std::string> CreateManifest ( const fs::path &Root )
{
    std::error_code Ec;
    if ( !fs::is_regular_file( Root / fs::path( WellKnown::DefaultEntry ), Ec ) )
    {
        FLogger::Error( "No entrypoint found: expected " + ( Root / fs::path( WellKnown::DefaultEntry ) ).string(), "circuit" );
        return std::nullopt;
    }

    FManifest Manifest;
    Manifest.Name       = Root.filename().string();
    Manifest.Runtime    = std::string( Volt::CLI::VoltVersion );
    Manifest.Entrypoint = std::string( WellKnown::DefaultEntry );
    Manifest.Modules    = ScanModules( Root );
    return FormatManifest( Manifest );
}

/// Existing manifest: preserve its entries, add newly discovered modules,
/// and flag (without deleting) whatever disappeared from disk.
[[nodiscard]] std::optional<std::string> UpdateManifest ( const fs::path &Root, const fs::path &ManifestPath )
{
    std::optional<FManifest> Manifest = ReadManifest( ManifestPath );
    if ( !Manifest.has_value() )
    {
        return std::nullopt;
    }

    std::error_code Ec;
    if ( !fs::is_regular_file( Root / fs::path( Manifest->Entrypoint ), Ec ) )
    {
        FLogger::Warn( "Entrypoint points to a missing file: " + Manifest->Entrypoint, "circuit" );
    }

    for ( const auto &[Name, Dir] : ScanModules( Root ) )
    {
        const bool bKnown =
            std::ranges::any_of( Manifest->Modules, [&Name] ( const auto &Entry ) { return Entry.first == Name; } );
        if ( !bKnown )
        {
            FLogger::Info( "New module discovered: " + Quote( Name ) + " => " + Quote( Dir ), "circuit" );
            Manifest->Modules.emplace_back( Name, Dir );
        }
    }

    for ( const auto &[Name, Dir] : Manifest->Modules )
    {
        if ( !fs::is_directory( Root / fs::path( Dir ), Ec ) )
        {
            FLogger::Warn( "Module " + Quote( Name ) + " points to a missing directory: " + Dir, "circuit" );
        }
    }

    return FormatManifest( *Manifest );
}

} // namespace

/**
 * Public
 */

std::string_view Volt::CLI::FCircuitCommand::GetName () const noexcept
{
    return "circuit";
}

std::string_view Volt::CLI::FCircuitCommand::GetDescription () const noexcept
{
    return "Create or update the Project.vl file";
}

std::string_view Volt::CLI::FCircuitCommand::GetUsage () const noexcept
{
    return "volt circuit [options]";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FCircuitCommand::GetOptions ()
{
    // clang-format off
    return {
        {
            "-d", "--dir", "DIR", "Project directory path",
            [this] ( std::string_view Val ) { this->ProjectDirectory = Val; }
        }
    };
    // clang-format on
}

std::int32_t Volt::CLI::FCircuitCommand::Execute ( std::span<const std::string_view> InArgs )
{
    const std::vector<FOption> Options = GetOptions();
    const auto Result                  = CommandParser::Parse( InArgs, Options );
    if ( not Result.has_value() )
    {
        Core::FLogger::Error( Result.error() );
        Core::FLogger::Flush();
        CommandParser::PrintUsage( std::cerr, GetUsage(), Options );
        return ExitFailure;
    }
    if ( Result->bHelpRequested )
    {
        Core::FLogger::Flush();
        CommandParser::PrintUsage( std::cout, GetUsage(), Options );
        return ExitSuccess;
    }

    if ( ProjectDirectory.empty() and not Result->Positionals.empty() )
    {
        ProjectDirectory = Result->Positionals.front();
    }

    std::error_code Ec;
    const fs::path Root = fs::absolute( ProjectDirectory.empty() ? "." : ProjectDirectory, Ec ).lexically_normal();
    if ( not fs::is_directory( Root, Ec ) )
    {
        Core::FLogger::Error( "Directory not found: " + Root.string(), "circuit" );
        return ExitFailure;
    }

    Core::FLogger::Info( "Configuring project metadata in: " + Root.string(), "circuit" );
    Core::FLogger::Progress( "Scanning workspace directories...", "1/2" );

    const fs::path ManifestPath = Root / WellKnown::ManifestName;
    const bool bExisting        = fs::is_regular_file( ManifestPath, Ec );

    const std::optional<std::string> Content = bExisting ? UpdateManifest( Root, ManifestPath ) : CreateManifest( Root );
    if ( not Content.has_value() )
    {
        Core::FLogger::Progress( "Synchronization aborted", "2/2", /*bFinished=*/true );
        return ExitFailure;
    }

    std::string Before;
    if ( bExisting )
    {
        std::ifstream Stream( ManifestPath, std::ios::binary );
        std::ostringstream Buffer;
        Buffer << Stream.rdbuf();
        Before = Buffer.str();
    }

    if ( Before != *Content )
    {
        std::ofstream Stream( ManifestPath, std::ios::binary | std::ios::trunc );
        if ( not Stream )
        {
            Core::FLogger::Error( "Cannot write '" + ManifestPath.string() + "'", "circuit" );
            return ExitFailure;
        }
        Stream << *Content;
    }

    const std::string Status = ( Before == *Content ) ? "already up to date" : "synchronized successfully";
    Core::FLogger::Progress( std::string( WellKnown::ManifestName ) + " " + Status, "2/2", /*bFinished=*/true );
    return ExitSuccess;
}

/**
 * Private
 */

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FCircuitCommand> RegisterCircuitCommand;

} // namespace
