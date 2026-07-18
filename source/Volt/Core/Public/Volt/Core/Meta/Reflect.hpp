#pragma once

/**
 * @file Reflect.hpp
 * @brief Field iteration over C++26 static reflection (P2996).
 * @details Nodes are plain aggregates. Every non-static data member is reflected
 *          except the ones named `Loc`, which carry source ranges shared by all nodes
 *          and are structural rather than semantic.
 */

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <variant>

#include <meta>

namespace Volt
{

namespace Meta
{

    namespace Detail
    {

        /**
         * @constant kHiddenField
         * @brief The one field name reflection skips: source-location plumbing.
         */
        inline constexpr std::string_view kHiddenField = "Loc";

    } // namespace Detail

    /**
     * @concept Reflected
     * @brief A type is Reflected when field iteration can walk it: any aggregate.
     */
    template <typename T>
    concept Reflected = std::is_aggregate_v<std::remove_cvref_t<T>>;

    // clang-format off

    /**
     * @function ForEachField
     * @brief Invoke Callback( Name, FieldRef ) for each semantic field, in declaration order.
     * @details This is what makes printer / clone / walk generic — no per-node code, and a field can no longer be forgotten.
     * @param Object The object to reflect over.
     * @param Callback The callback function to invoke for each field.
     */
    template <Reflected T, typename Fn> constexpr void ForEachField ( T &Object, Fn &&Callback );

    /// Number of semantic fields ForEachField will visit.
    template <Reflected T> [[nodiscard]] consteval std::size_t FieldCount ();

    /// Enumerator name of Value, or "<unknown>" for out-of-range values.
    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr std::string_view EnumName ( E Value );

    /// Type name of the active alternative of a variant (monostate → "None").
    template <typename... Alts> [[nodiscard]] constexpr std::string_view ActiveName ( const std::variant<Alts...> &Node );

    // clang-format on

} // namespace Meta

} // namespace Volt

#include "Inline/Reflect.inl"
