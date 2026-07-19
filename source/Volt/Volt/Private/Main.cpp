#include "Volt/CLI/CommandRegistry.hpp"
#include "Volt/Core/Log/Logger.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

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
