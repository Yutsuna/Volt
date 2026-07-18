#pragma once

#include "Volt/CLI/CommandRegistry.hpp"

#include <memory>
#include <string_view>
#include <unordered_map>

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

        using FCommandMap = std::unordered_map<std::string_view, std::unique_ptr<IGenericCommand>>;

    public:

        /** @brief Get the Singleton instance */
        static FCommandRegistry &GetInstance () noexcept;

        /** @brief Register a command from an IGenericCommand pointer */
        void RegisterCommand ( std::unique_ptr<IGenericCommand> InCommand );

    public:

        /** @brief Get all registered commands */
        [[nodiscard]] const FCommandMap &GetRegisteredCommands () const noexcept;

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
            FCommandRegistry::GetInstance().RegisterCommand( std::make_unique<TCommand> );
        }
    };

} // namespace CLI

} // namespace Volt
