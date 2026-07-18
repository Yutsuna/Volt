#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Driver/Driver.hpp"

#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/**
 * Legacy invocation — `Volt [--print] <file.vl> [more...]` and
 * `Volt [--print] --circuit <Project.vl>` — kept verbatim because
 * scripts/golden.sh captures this exact surface (printed AST, diagnostics,
 * summary, exit code). New workflows go through the command registry.
 */
int RunLegacy ( std::span<const std::string_view> Args )
{
    std::vector<std::string> Files;
    bool bCircuit = false;
    bool bPrint   = false;

    for ( const std::string_view Arg : Args )
    {
        if ( Arg == "--circuit" )
        {
            bCircuit = true;
        }
        else if ( Arg == "--print" )
        {
            bPrint = true;
        }
        else
        {
            Files.emplace_back( Arg );
        }
    }

    if ( Files.empty() )
    {
        std::cerr << "usage: Volt [--print] <file.vl|file.vlx> [more...]\n"
                  << "       Volt [--print] --circuit <Project.vl>\n";
        return 2;
    }

    Volt::Driver::Driver Driver;
    const Volt::Driver::CompileResult Result = bCircuit ? Driver.CompileCircuit( Files.front() ) : Driver.CompileFiles( Files );

    if ( bPrint )
    {
        Driver.PrintUnits( std::cout );
    }

    Driver.RenderDiagnostics( std::cerr );

    std::cerr << "[Volt] " << Result.Files << " file(s), " << Result.Errors << " error(s)";
    if ( Result.JsxLowered > 0 )
    {
        std::cerr << ", " << Result.JsxLowered << " jsx node(s) lowered";
    }
    if ( Driver.Interfaces().Size() > 0 )
    {
        std::cerr << ", " << Driver.Interfaces().Size() << " exported decl(s)";
    }
    if ( bCircuit )
    {
        std::cerr << ", " << Driver.Graph().ModuleCount() << " module(s)";
        if ( Result.bCycle )
        {
            std::cerr << ", dependency cycle";
        }
    }
    std::cerr << '\n';

    return Driver.HasErrors() ? 1 : 0;
}

int Run ( std::span<const std::string_view> Args )
{
    using Volt::CLI::FCommandRegistry;
    using Volt::Core::FLogger;

    const FCommandRegistry &Registry = FCommandRegistry::GetInstance();

    if ( Args.empty() )
    {
        FLogger::Error( "No command specified." );
        FLogger::Error( "Available commands: " + Registry.JoinCommandNames() );
        return 84;
    }

    if ( Volt::CLI::IGenericCommand *Command = Registry.Find( Args.front() ) )
    {
        return Command->Execute( Args.subspan( 1 ) );
    }

    // Not a command: fall back to the legacy direct-compile surface when the
    // arguments look like it (flags or an existing path) — golden.sh relies on it.
    std::error_code Ec;
    if ( Args.front().starts_with( '-' ) or std::filesystem::exists( Args.front(), Ec ) )
    {
        return RunLegacy( Args );
    }

    FLogger::Error( "Unknown command: " + std::string( Args.front() ) );
    FLogger::Error( "Available commands: " + Registry.JoinCommandNames() );
    return 84;
}

} // namespace

// Front CLI: dispatch to the registered command (parse/check/circuit/help...),
// which drives the Driver pipeline and reports through the async logger.
int main ( int ArgCount, const char **ArgValues )
{
    const Volt::Core::FLogScope LogScope;

    std::vector<std::string_view> Args;
    Args.reserve( static_cast<std::size_t>( ArgCount > 0 ? ArgCount - 1 : 0 ) );
    for ( int Index = 1; Index < ArgCount; ++Index )
    {
        Args.emplace_back( ArgValues[Index] );
    }

    const int Code = Run( Args );
    Volt::Core::FLogger::Flush();
    return Code;
}
