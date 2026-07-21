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
    ///
    /// Under VOLT_CHECKED_IDS (opt-in build config), Ids also carry the tag
    /// of the arena that minted them so Arena::Get can catch an Id crossing
    /// into a foreign arena. Provenance never participates in comparison or
    /// hashing, and the field does not exist in normal builds.
    template <typename Tag> struct TypedId
    {

        using ValueType = std::uint32_t;

        static constexpr ValueType InvalidValue = ~ValueType{ 0 };

        ValueType Value = InvalidValue;

#if defined( VOLT_CHECKED_IDS )
        // 0 = unstamped (hand-built index): bounds-checked only.
        ValueType Provenance = 0;
#endif

        constexpr TypedId () = default;

        constexpr explicit TypedId ( ValueType InValue ) : Value( InValue )
        {
        }

        [[nodiscard]] constexpr bool IsValid () const
        {
            return Value != InvalidValue;
        }

        [[nodiscard]] constexpr explicit operator bool () const
        {
            return IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>( const TypedId &Other ) const
        {
            return Value <=> Other.Value;
        }

        [[nodiscard]] constexpr bool operator==( const TypedId &Other ) const
        {
            return Value == Other.Value;
        }
    };

} // namespace Core

} // namespace Volt

namespace std
{

template <typename Tag> struct hash<Volt::Core::TypedId<Tag>>
{

    std::size_t operator()( const Volt::Core::TypedId<Tag> &Id ) const noexcept
    {
        return std::hash<std::uint32_t>{}( Id.Value );
    }
};

} // namespace std
