#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace Volt
{

    namespace Core
    {

        /// Strongly-typed 32-bit index into an Arena. The Tag makes every Id
        /// family (ExprId, StmtId, ...) a distinct, non-interchangeable type.
        template <typename Tag>
        struct TypedId
        {

            using ValueType = std::uint32_t;

            static constexpr ValueType InvalidValue = ~ValueType{ 0 };

            ValueType Value = InvalidValue;

            constexpr TypedId() = default;

            constexpr explicit TypedId( ValueType InValue ) : Value( InValue )
            {
            }

            [[nodiscard]] constexpr bool IsValid() const
            {
                return Value != InvalidValue;
            }

            [[nodiscard]] constexpr explicit operator bool() const
            {
                return IsValid();
            }

            constexpr auto operator<=>( const TypedId& ) const = default;
        };

    }

}

namespace std
{

    template <typename Tag>
    struct hash<Volt::Core::TypedId<Tag>>
    {

        std::size_t operator()( const Volt::Core::TypedId<Tag>& Id ) const noexcept
        {
            return std::hash<std::uint32_t>{}( Id.Value );
        }
    };

}
