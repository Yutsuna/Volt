#pragma once

#include <cstddef>
#include <variant>

namespace Volt
{

namespace Meta
{

    /**
     * @struct TypeList
     * @brief A compile-time list of types.
     * @details Used to turn a node manifest into a std::variant
     *          and to index alternatives by their Kind enum.
     */
    template <typename... Ts> struct TypeList
    {

        static constexpr std::size_t Size = sizeof...( Ts );
    };

    /** @struct AsVariant */
    template <typename List> struct AsVariant;

    /** @brief Specialization of AsVariant for TypeList */
    template <typename... Ts> struct AsVariant<TypeList<Ts...>>
    {

        using Type = std::variant<Ts...>;
    };

    /** @brief Turn a TypeList<...> into the corresponding std::variant<...>. */
    template <typename List> using AsVariantT = typename AsVariant<List>::Type;

    /** @struct IndexOf */
    template <typename T, typename List> struct IndexOf;

    /** @brief Found: T is the head of the list */
    template <typename T, typename... Tail> struct IndexOf<T, TypeList<T, Tail...>>
    {

        static constexpr std::size_t Value = 0;
    };

    /** @brief Not yet: skip the head and keep searching */
    template <typename T, typename Head, typename... Tail> struct IndexOf<T, TypeList<Head, Tail...>>
    {

        static constexpr std::size_t Value = 1 + IndexOf<T, TypeList<Tail...>>::Value;
    };

    /** @brief Not found: T is not in the list */
    template <typename T, typename List> inline constexpr std::size_t IndexOfV = IndexOf<T, List>::Value;

} // namespace Meta

} // namespace Volt
