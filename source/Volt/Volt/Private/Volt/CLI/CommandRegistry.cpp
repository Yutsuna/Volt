#include "Volt/CLI/CommandRegistry.hpp"

#include <utility>

/**
 * Public
 */

Volt::CLI::FCommandRegistry &Volt::CLI::FCommandRegistry::GetInstance () noexcept
{
    static FCommandRegistry Instance;
    return Instance;
}

void Volt::CLI::FCommandRegistry::RegisterCommand ( std::unique_ptr<IGenericCommand> InCommand )
{
    const std::string_view Name = InCommand->GetName();
    Commands.emplace( Name, std::move( InCommand ) );
}

const Volt::CLI::FCommandRegistry::FCommandMap &Volt::CLI::FCommandRegistry::GetRegisteredCommands () const noexcept
{
    return Commands;
}

Volt::CLI::IGenericCommand *Volt::CLI::FCommandRegistry::Find ( std::string_view InName ) const noexcept
{
    const auto It = Commands.find( InName );
    return It == Commands.end() ? nullptr : It->second.get();
}

std::string Volt::CLI::FCommandRegistry::JoinCommandNames ( std::string_view InSeparator ) const
{
    std::string Joined;
    for ( const auto &[Name, Command] : Commands )
    {
        if ( !Joined.empty() )
        {
            Joined += InSeparator;
        }
        Joined += Name;
    }
    return Joined;
}
