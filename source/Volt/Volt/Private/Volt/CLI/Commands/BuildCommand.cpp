#include "Volt/CLI/Commands/BuildCommand.hpp"
#include "Volt/CLI/CommandParser.hpp"
#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Driver/Driver.hpp"
#include "Volt/Driver/WellKnown.hpp"

#ifdef VOLT_ENABLE_LLVM
    #include "Volt/BackendCore/TargetBackend.hpp"
    #include "Volt/BackendLLVM/LlvmEmitter.hpp"
#endif

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

/**
 * Private helpers
 */

namespace
{

namespace fs = std::filesystem;

#ifdef VOLT_ENABLE_LLVM
/// Walk up from InPath (file or directory) looking for a Project.vl manifest.
/// Mirrors FCheckCommand's DiscoverManifest: a file inside a circuit project
/// selects the whole circuit.
[[nodiscard]] std::optional<fs::path> DiscoverManifest ( const fs::path &InPath )
{
    std::error_code Ec;
    fs::path Dir = fs::absolute( fs::is_directory( InPath, Ec ) ? InPath : InPath.parent_path(), Ec );

    while ( !Dir.empty() )
    {
        fs::path Candidate = Dir / Volt::Driver::WellKnown::ManifestName;
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
#endif

} // namespace

/**
 * Public
 */

std::string_view Volt::CLI::FBuildCommand::GetName () const noexcept
{
    return "build";
}

std::string_view Volt::CLI::FBuildCommand::GetDescription () const noexcept
{
    return "Compile the file input";
}

std::string_view Volt::CLI::FBuildCommand::GetUsage () const noexcept
{
    return "volt build [options] [input_file]";
}

std::vector<Volt::CLI::FOption> Volt::CLI::FBuildCommand::GetOptions ()
{
    // clang-format off
    return {
        {
            "-i", "--input", "INPUT", "File input source program",
            [this] ( std::string_view Val ) { this->Input = Val; }
        },
        {
            "-o", "--output", "OUTPUT", "Output artifact path",
            [this] ( std::string_view Val ) { this->Output = Val; }
        },
        {
            "", "--target", "TARGET", "Code generation target (native|wasm)",
            [this] ( std::string_view Val ) { this->Target = Val; }
        },
        {
            "-O", "", "LEVEL", "Optimization level (0|2|3)",
            [this] ( std::string_view Val ) { this->OptLevel = Val; }
        },
        {
            "", "--emit", "KIND", "Stop after an intermediate artifact (ir|obj)",
            [this] ( std::string_view Val ) { this->Emit = Val; }
        },
        {
            "", "--lto", "", "Enable link-time optimization (native only)",
            [this] ( std::string_view ) { this->bLto = true; }
        }
    };
    // clang-format on
}

std::int32_t Volt::CLI::FBuildCommand::Execute ( std::span<const std::string_view> InArgs )
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

    if ( Input.empty() and not Result->Positionals.empty() )
    {
        Input = Result->Positionals.front();
    }
    else if ( not Input.empty() and not Result->Positionals.empty() )
    {
        Core::FLogger::Error( "Unexpected argument: " + std::string( Result->Positionals.front() ), "build" );
        Core::FLogger::Flush();
        CommandParser::PrintUsage( std::cerr, GetUsage(), Options );
        return ExitFailure;
    }
    if ( Input.empty() )
    {
        std::error_code Ec;
        if ( fs::is_regular_file( fs::current_path( Ec ) / Driver::WellKnown::ManifestName, Ec ) )
        {
            Input = fs::current_path( Ec ).string();
        }
    }
    if ( Input.empty() )
    {
        Core::FLogger::Error( "Missing source input (-i)", "build" );
        return ExitFailure;
    }

    std::error_code Ec;
    if ( not fs::exists( Input, Ec ) )
    {
        Core::FLogger::Error( "Cannot read '" + Input + "': no such file or directory", "build" );
        return ExitFailure;
    }

    if ( Target != "native" )
    {
        Core::FLogger::Error( "Unsupported --target '" + Target + "': only 'native' is implemented", "build" );
        return ExitFailure;
    }

#ifndef VOLT_ENABLE_LLVM
    Core::FLogger::Error( "This build of volt was configured without LLVM (VOLT_ENABLE_LLVM=OFF); "
                          "'volt build' is unavailable",
                          "build" );
    return ExitFailure;
#else
    Driver::Driver TheDriver;
    Driver::CompileResult Compiled;

    if ( const std::optional<fs::path> Manifest = DiscoverManifest( Input ) )
    {
        Core::FLogger::Info( "Circuit manifest found: " + Manifest->string(), "build" );
        Core::FLogger::Progress( "Compiling...", "build" );
        Compiled = TheDriver.CompileCircuit( Manifest->string() );
    }
    else
    {
        Core::FLogger::Progress( "Compiling...", "build" );
        Compiled = TheDriver.CompileFiles( { Input } );
    }

    Core::FLogger::Progress( TheDriver.HasErrors() ? "Compilation failed" : "Compilation complete", "build",
                             /*bFinished=*/true );

    if ( TheDriver.DiagnosticCount() > 0 )
    {
        Core::FLogger::Flush();
        TheDriver.RenderDiagnostics( std::cerr );
    }

    if ( TheDriver.HasErrors() )
    {
        Core::FLogger::Error(
            std::to_string( Compiled.Errors ) + " error(s) across " + std::to_string( Compiled.Files ) + " file(s)", "build" );
        return ExitFailure;
    }

    Backend::Llvm::EmitOptions EmitOpts;
    EmitOpts.OutputPath = Output;
    EmitOpts.bLto       = bLto;

    if ( Emit == "ir" )
    {
        EmitOpts.Stage = Backend::Llvm::EEmitStage::Ir;
    }
    else if ( Emit == "obj" )
    {
        EmitOpts.Stage = Backend::Llvm::EEmitStage::Object;
    }
    else if ( not Emit.empty() )
    {
        Core::FLogger::Error( "Unsupported --emit '" + Emit + "': expected 'ir' or 'obj'", "build" );
        return ExitFailure;
    }

    if ( not OptLevel.empty() )
    {
        if ( OptLevel == "0" )
        {
            EmitOpts.OptLevel = 0;
        }
        else if ( OptLevel == "2" )
        {
            EmitOpts.OptLevel = 2;
        }
        else if ( OptLevel == "3" )
        {
            EmitOpts.OptLevel = 3;
        }
        else
        {
            Core::FLogger::Error( "Unsupported -O '" + OptLevel + "': expected 0, 2 or 3", "build" );
            return ExitFailure;
        }
    }

    Backend::Llvm::LlvmBackend LlvmImpl;
    LlvmImpl.SetOptions( EmitOpts );

    std::unique_ptr<Backend::IBackend> TheBackend = Backend::MakeBackend( std::move( LlvmImpl ) );

    const std::vector<Backend::UnitView> Views = TheDriver.MakeBackendViews();
    const Backend::BackendInput BackendIn{ .Types = &TheDriver.MutableLayouts(), .Units = Views };
    TheBackend->Begin( BackendIn );

    // A unit that fails leaves State::Failed() set, so every later EmitUnit
    // is a guarded no-op; Finalize() is still what surfaces the real message.
    for ( const Backend::UnitView &Unit : Views )
    {
        static_cast<void>( TheBackend->EmitUnit( Unit ) );
    }

    const Backend::EmitResult Emitted = TheBackend->Finalize();
    if ( Emitted.Status != Backend::EEmitStatus::Ok )
    {
        Core::FLogger::Error( "Finalize failed: " + Emitted.Message, "build" );
        return ExitFailure;
    }

    Core::FLogger::Info( "OK : " + Emitted.Artifact, "build" );
    return ExitSuccess;
#endif
}

/**
 * Private
 */

namespace
{

const Volt::CLI::TCommandRegister<Volt::CLI::FBuildCommand> RegisterBuildCommand;

} // namespace
