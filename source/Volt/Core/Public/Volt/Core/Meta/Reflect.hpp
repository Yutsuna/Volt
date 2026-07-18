#pragma once

// Reflect.hpp — field iteration over C++26 static reflection (P2996).
// Nodes are plain aggregates: no VOLT_FIELDS macro, no `using Self`, no
// per-node registration. Every non-static data member is reflected except
// the ones named `Loc`, which carry source ranges shared by all nodes and
// are structural rather than semantic.

#include <cstddef>
#include <meta>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Volt
{

namespace Meta
{

    namespace Detail
    {

        /// The one field name reflection skips: source-location plumbing.
        inline constexpr std::string_view HiddenField = "Loc";

    } // namespace Detail

    /// A type is Reflected when field iteration can walk it: any aggregate.
    template <typename T>
    concept Reflected = std::is_aggregate_v<std::remove_cvref_t<T>>;

    // clang-format off

    /// Invoke Callback( Name, FieldRef ) for each semantic field, in
    /// declaration order. This is what makes printer / clone / walk generic —
    /// no per-node code, and a field can no longer be forgotten.
    template <Reflected T, typename Fn> constexpr void ForEachField ( T &Object, Fn &&Callback )
    {
        using Bare = std::remove_cvref_t<T>;
        static constexpr auto Members = std::define_static_array( std::meta::nonstatic_data_members_of( ^^Bare, std::meta::access_context::unchecked() ) );
        template for ( constexpr auto Member : Members )
        {
            if constexpr ( std::meta::identifier_of( Member ) != Detail::HiddenField )
            {
                Callback( std::define_static_string( std::meta::identifier_of( Member ) ), Object.[:Member:] );
            }
        }
    }

    /// Number of semantic fields ForEachField will visit.
    template <Reflected T> [[nodiscard]] consteval std::size_t FieldCount ()
    {
        std::size_t Count = 0;
        for ( const auto Member : std::meta::nonstatic_data_members_of( ^^std::remove_cvref_t<T>, std::meta::access_context::unchecked() ) )
        {
            if ( std::meta::identifier_of( Member ) != Detail::HiddenField )
            {
                ++Count;
            }
        }
        return Count;
    }

    /// Enumerator name of Value, or "<unknown>" for out-of-range values.
    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr std::string_view EnumName ( E Value )
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

    /// Type name of the active alternative of a variant (monostate → "None").
    template <typename... Alts> [[nodiscard]] constexpr std::string_view ActiveName ( const std::variant<Alts...> &Node )
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
                    // ^^T reflects the local alias; dealias to the node type.
                    return std::meta::identifier_of( std::meta::dealias( ^^T ) );
                }
            },
            Node );
    }

    // clang-format on

} // namespace Meta

} // namespace Volt
