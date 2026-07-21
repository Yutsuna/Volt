#pragma once

#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Sema/Layout/MemoryLayout.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>

namespace Volt
{

namespace Sema
{

    // Owns every resolved MemoryLayout and the name -> layout bindings that
    // the TypeChecker will populate from the Volt stdlib + annotations. The
    // store is deliberately empty at construction: there are no built-in
    // types baked into C++ — `Int`, `String`, `Array[T]` all arrive through
    // Bind() once their Volt definitions are resolved (a later phase).
    class TypeStore
    {

    public:

        [[nodiscard]] LayoutId AddPrimitive ( Symbol Spelling, std::uint32_t Bits )
        {
            return Layouts.Add( LayoutNode{ Primitive{ Spelling, Bits } } );
        }

        [[nodiscard]] LayoutId AddPointer ( LayoutId Pointee )
        {
            return Layouts.Add( LayoutNode{ Pointer{ Pointee } } );
        }

        [[nodiscard]] LayoutId AddAggregate ( Aggregate Node )
        {
            return Layouts.Add( LayoutNode{ std::move( Node ) } );
        }

        [[nodiscard]] const LayoutNode &Get ( LayoutId Id ) const
        {
            return Layouts.Get( Id );
        }

        [[nodiscard]] std::size_t Size () const
        {
            return Layouts.Size();
        }

        // Bind a resolved name (interned, possibly qualified as "Core::App")
        // to its layout. Re-binding the same name overwrites, matching the
        // "last definition wins" resolution order of the stdlib pass.
        void Bind ( Symbol Name, LayoutId Layout )
        {
            ByName.insert_or_assign( Name, Layout );
        }

        [[nodiscard]] std::optional<LayoutId> Lookup ( Symbol Name ) const
        {
            if ( const auto It = ByName.find( Name ); It != ByName.end() )
            {
                return It->second;
            }
            return std::nullopt;
        }

    private:

        Core::Arena<LayoutNode, LayoutId> Layouts;
        std::unordered_map<Symbol, LayoutId> ByName;
    };

} // namespace Sema

} // namespace Volt
