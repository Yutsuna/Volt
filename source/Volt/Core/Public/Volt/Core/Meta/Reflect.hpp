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
