#pragma once

#include <cstddef>
#include <type_traits>
#include <variant>

namespace Volt
{

namespace Meta
{

    /// Compile-time list of types. Used to turn a node manifest into a
    /// std::variant and to index alternatives by their Kind enum.
    template <typename... Ts> struct TypeList
    {

        static constexpr std::size_t Size = sizeof...( Ts );
    };

    template <typename List> struct AsVariant;

    template <typename... Ts> struct AsVariant<TypeList<Ts...>>
    {

        using Type = std::variant<Ts...>;
    };

    /// Turn a TypeList<...> into the corresponding std::variant<...>.
    template <typename List> using AsVariantT = typename AsVariant<List>::Type;

    template <typename T, typename List> struct IndexOf;

    // Found: T is the head of the list.
    template <typename T, typename... Tail> struct IndexOf<T, TypeList<T, Tail...>>
    {

        static constexpr std::size_t Value = 0;
    };

    // Not yet: skip the head and keep searching (this specialisation is
    // less specialised than the one above, so it only wins when T != Head).
    template <typename T, typename Head, typename... Tail> struct IndexOf<T, TypeList<Head, Tail...>>
    {

        static constexpr std::size_t Value = 1 + IndexOf<T, TypeList<Tail...>>::Value;
    };

    template <typename T, typename List> inline constexpr std::size_t IndexOfV = IndexOf<T, List>::Value;

} // namespace Meta

} // namespace Volt
