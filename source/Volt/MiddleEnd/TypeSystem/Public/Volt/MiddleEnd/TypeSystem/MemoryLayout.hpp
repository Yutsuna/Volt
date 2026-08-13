#pragma once

#include "Volt/Core/Support/Id.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Core/Support/StringInterner.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace Volt
{

namespace MiddleEnd
{

    namespace TypeSystem
    {

        // A layout is referenced by a strongly-typed handle into the TypeStore,
        // never a pointer — same value-arena discipline as the AST.
        struct LayoutTag
        {
        };

        using LayoutId = ::Volt::Core::TypedId<LayoutTag>;

        using ::Volt::Core::Symbol;

        // The C++ compiler describes a scalar only by an opaque spelling and a
        // bit width. `Int32` / `String` are never mentioned here: the spelling
        // comes verbatim from the Volt stdlib's `@[Primitive("i32")]`, so the
        // set of primitives is open and lives entirely in Volt.
        struct Primitive
        {

            Symbol Spelling; // interned "i32", "i1", "ptr", ...
            std::uint32_t Bits = 0;
        };

        // A pointer to another layout — `Pointer[T]`, `T*`, or an aggregate's
        // reference field once lowered.
        struct Pointer
        {

            LayoutId Pointee;
        };

        // One field of an aggregate: a name plus the layout it resolves to.
        struct FieldLayout
        {

            Symbol Name;
            LayoutId Type;
        };

        // A composite of fields laid out in declaration order. `String`,
        // `Array[T]` and every user struct/class collapse to this once their
        // Volt definition is resolved.
        struct Aggregate
        {

            ::Volt::Core::SmallVec<FieldLayout, 4> Fields;
        };

        using LayoutNode = std::variant<std::monostate, Primitive, Pointer, Aggregate>;

        enum class LayoutKind : std::uint8_t
        {

            None      = 0,
            Primitive = 1,
            Pointer   = 2,
            Aggregate = 3,
        };

        [[nodiscard]] constexpr LayoutKind KindOf ( const LayoutNode &Node )
        {
            return static_cast<LayoutKind>( Node.index() );
        }

        [[nodiscard]] constexpr std::string_view LayoutName ( LayoutKind Kind )
        {
            switch ( Kind )
            {
            case LayoutKind::None:
                return "None";
            case LayoutKind::Primitive:
                return "Primitive";
            case LayoutKind::Pointer:
                return "Pointer";
            case LayoutKind::Aggregate:
                return "Aggregate";
            }
            return "?";
        }

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
