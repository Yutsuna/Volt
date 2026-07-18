#pragma once

#include "Volt/CLI/GenericCommand.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace Volt
{

namespace CLI
{

    /**
     * @class FCommandRegistry
     * @brief Singleton class that manages the registration and retrieval of CLI commands.
     */
    class FCommandRegistry
    {
    public:

        // Ordered by name so `volt help` and error listings are deterministic.
        using FCommandMap = std::map<std::string_view, std::unique_ptr<IGenericCommand>>;

    public:

        /** @brief Get the Singleton instance */
        static FCommandRegistry &GetInstance () noexcept;

        /** @brief Register a command from an IGenericCommand pointer */
        void RegisterCommand ( std::unique_ptr<IGenericCommand> InCommand );

    public:

        /** @brief Get all registered commands */
        [[nodiscard]] const FCommandMap &GetRegisteredCommands () const noexcept;

        /** @brief Find a command by name, or nullptr */
        [[nodiscard]] IGenericCommand *Find ( std::string_view InName ) const noexcept;

        /** @brief The registered command names joined by InSeparator */
        [[nodiscard]] std::string JoinCommandNames ( std::string_view InSeparator = ", " ) const;

    private:

        FCommandRegistry () noexcept = default;

    private:

        FCommandMap Commands;
    };

    /**
     * @class TCommandRegister
     * @brief Template class for registering a command of type TCommand with the command registry.
     * @tparam TCommand The type of the command to register, which must derive from IGenericCommand.
     */
    template <typename TCommand> class TCommandRegister
    {
    public:

        TCommandRegister () noexcept
        {
            FCommandRegistry::GetInstance().RegisterCommand( std::make_unique<TCommand>() );
        }
    };

} // namespace CLI

} // namespace Volt
