#pragma once

#include "Volt/Core/Meta/Reflect.hpp"

// clang-format off

template <Reflected T, typename Fn> constexpr void Volt::Meta::ForEachField ( T &Object, Fn &&Callback )
{
    using Bare = std::remove_cvref_t<T>;
    static constexpr auto Members = std::define_static_array( std::meta::nonstatic_data_members_of( ^^Bare, std::meta::access_context::unchecked() ) );
    template for ( constexpr auto Member : Members )
    {
        if constexpr ( std::meta::identifier_of( Member ) != Detail::kHiddenField )
        {
            Callback( std::define_static_string( std::meta::identifier_of( Member ) ), Object.[:Member:] );
        }
    }
}

template <Reflected T> [[nodiscard]] consteval std::size_t Volt::Meta::FieldCount ()
{
    std::size_t Count = 0;
    for ( const auto Member : std::meta::nonstatic_data_members_of( ^^std::remove_cvref_t<T>, std::meta::access_context::unchecked() ) )
    {
        if ( std::meta::identifier_of( Member ) != Detail::kHiddenField )
        {
            ++Count;
        }
    }
    return Count;
}

template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr std::string_view Volt::Meta::EnumName ( E Value )
{
    template for ( constexpr auto Enumerator : std::define_static_array( std::meta::enumerators_of( ^^E ) ) )
    {
        if ( Value == [:Enumerator:] )
        {
            return std::meta::identifier_of( Enumerator );
        }
    }
    return "<unknown>";
}

template <typename... Alts> [[nodiscard]] constexpr std::string_view Volt::Meta::ActiveName ( const std::variant<Alts...> &Node )
{
    return std::visit(
        [] ( const auto &Alt ) -> std::string_view
        {
            using T = std::remove_cvref_t<decltype( Alt )>;
            if constexpr ( std::same_as<T, std::monostate> )
            {
                return "None";
            }
            else
            {
                /** INFO: ^^T reflects the local alias; dealias to the node type. */
                return std::meta::identifier_of( std::meta::dealias( ^^T ) );
            }
        },
        Node );
}

// clang-format on
