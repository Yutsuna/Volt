#pragma once

#include "Volt/Core/Meta/Manifest.hpp"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Volt
{

namespace Meta
{

    /// One reflected field: its source name plus a pointer-to-member. CTAD
    /// deduces the owning class and field type from `Field{ "x", &S::x }`.
    template <typename Class, typename Member> struct Field
    {

        const char *Name;
        Member Class::*Ptr;
    };

    /// A struct is Reflected when it declared its fields with VOLT_FIELDS.
    template <typename T>
    concept Reflected = requires { T::ReflectFields(); };

    /// Invoke Callback( Name, FieldRef ) for each declared field, in order.
    /// This is what makes printer / clone / walk generic — no per-node code.
    template <typename T, typename Fn> constexpr void ForEachField ( T &Object, Fn &&Callback )
    {
        std::apply( [&] ( auto... Fields ) { ( Callback( Fields.Name, Object.*( Fields.Ptr ) ), ... ); }, std::remove_cvref_t<T>::ReflectFields() );
    }

    template <typename T> [[nodiscard]] constexpr std::size_t FieldCount ()
    {
        return std::tuple_size_v<decltype( std::remove_cvref_t<T>::ReflectFields() )>;
    }

    namespace Detail
    {

        /// Converts to any member type; only ever named in unevaluated context.
        struct AnyField
        {

            template <typename T> constexpr operator T () const noexcept;
        };

        template <typename T, typename... Probes> [[nodiscard]] consteval std::size_t AggregateArityImpl ()
        {
            if constexpr ( requires { T{ Probes{}..., AnyField{} }; } )
            {
                return AggregateArityImpl<T, Probes..., AnyField>();
            }
            else
            {
                return sizeof...( Probes );
            }
        }

    } // namespace Detail

    /// Number of direct members of an aggregate, counted by probing brace
    /// initialisation. Pairs with FieldCount to catch a field added to a
    /// struct but forgotten in VOLT_FIELDS — the silent-desync failure mode.
    /// Stopgap until P2996 reflection lands in mainline compilers, at which
    /// point ForEachField is reimplemented over std::meta and VOLT_FIELDS dies.
    template <typename T> [[nodiscard]] consteval std::size_t AggregateArity ()
    {
        static_assert( std::is_aggregate_v<std::remove_cvref_t<T>> );
        return Detail::AggregateArityImpl<std::remove_cvref_t<T>>();
    }

} // namespace Meta

} // namespace Volt

// Declare a node's reflectable fields. Expects `using Self = <ThisStruct>;`
// to be visible. Adds ReflectFields(): a tuple of Field{ name, member-ptr }.
#define VOLT_FIELD_ENTRY( NAME )                                                                                                                                         \
    ::Volt::Meta::Field                                                                                                                                                  \
    {                                                                                                                                                                    \
        VOLT_STRINGIFY( NAME ), &Self::NAME                                                                                                                              \
    }

#define VOLT_FIELDS( ... )                                                                                                                                               \
    static constexpr auto ReflectFields()                                                                                                                                \
    {                                                                                                                                                                    \
        return std::make_tuple( VOLT_FOR_EACH( VOLT_FIELD_ENTRY, __VA_ARGS__ ) );                                                                                        \
    }

// A node with no reflectable fields (e.g. NilLiteral) still needs the hook.
#define VOLT_FIELDS_NONE()                                                                                                                                               \
    static constexpr auto ReflectFields()                                                                                                                                \
    {                                                                                                                                                                    \
        return std::make_tuple();                                                                                                                                        \
    }
