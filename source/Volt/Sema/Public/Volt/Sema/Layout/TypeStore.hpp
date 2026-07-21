#pragma once

#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Sema/Layout/MemoryLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Volt
{

namespace Sema
{

    // A declared Volt type, referenced by handle like everything else.
    struct NominalTag
    {
    };

    using NominalId = Core::TypedId<NominalTag>;

    // One type as the compiler knows it: a name, where it was declared, and
    // — only once resolved — the memory layout it collapses to.
    //
    // Identity is *nominal*, never structural: two structs with identical
    // fields share a layout but are different types, and a type's methods
    // are found through its declaration, not its shape. Type checking works
    // entirely at this level; Layout stays invalid until something actually
    // needs a memory representation (codegen), and never for a generic whose
    // shape depends on its arguments.
    struct NominalType
    {

        Symbol Name;            // interned in the store's own table
        std::uint32_t Unit = 0; // ordinal of the declaring CompileUnit
        Frontend::DeclId Decl;  // its declaration, for member lookup
        LayoutId Layout;        // invalid until resolved
    };

    // Owns every declared type, the layouts they resolve to, and the
    // literal-kind bindings that give an untyped `10` a type. Deliberately
    // empty at construction: there are no built-in types baked into C++ —
    // `Int32`, `String`, `Array[T]` all arrive from the Volt stdlib through
    // DeclareType() + the `@[Primitive]` / `@[Literal]` annotations.
    //
    // The store is *per build*, not per file: a user file's `10` resolves to
    // the type declared in source/Lib/, and a CompileUnit's Symbols are
    // meaningless outside it. Hence its own interner and text-based keys.
    class TypeStore
    {

    public:

        [[nodiscard]] Symbol Intern ( std::string_view Text )
        {
            return Strings.Intern( Text );
        }

        [[nodiscard]] std::string_view Text ( Symbol Handle ) const
        {
            return Strings.Resolve( Handle );
        }

        // --- Types -------------------------------------------------------

        // Declare (or re-declare) a named type. Re-declaring a name returns
        // the existing handle and refreshes its origin, so the last stdlib
        // definition wins without invalidating handles already handed out.
        [[nodiscard]] NominalId DeclareType ( std::string_view Name, std::uint32_t Unit, Frontend::DeclId Decl )
        {
            const Symbol Key = Strings.Intern( Name );
            if ( const auto It = ByName.find( Key ); It != ByName.end() )
            {
                NominalType &Existing = Types.Get( It->second );
                Existing.Unit         = Unit;
                Existing.Decl         = Decl;
                return It->second;
            }

            const NominalId Id = Types.Add( NominalType{ .Name = Key, .Unit = Unit, .Decl = Decl, .Layout = LayoutId{} } );
            ByName.emplace( Key, Id );
            return Id;
        }

        void AttachLayout ( NominalId Id, LayoutId Layout )
        {
            Types.Get( Id ).Layout = Layout;
        }

        [[nodiscard]] const NominalType &Type ( NominalId Id ) const
        {
            return Types.Get( Id );
        }

        [[nodiscard]] std::optional<NominalId> LookupType ( std::string_view Name ) const
        {
            return Find( ByName, Name );
        }

        [[nodiscard]] std::size_t TypeCount () const
        {
            return Types.Size();
        }

        // --- Literals ----------------------------------------------------

        // Bind a literal kind ("IntLiteral", "StringLiteral", ...) to the
        // type that wraps it. `10` is an Int32 because Int32 — and only
        // Int32 — carries `@[Literal( IntLiteral )]`. The C++ side never
        // learns what "Int32" means, only that this type claimed that kind.
        //
        // Returns false when the kind was already claimed by a *different*
        // type, so the binder can report the ambiguity instead of letting
        // stdlib file order silently decide the type of every integer.
        bool BindLiteral ( std::string_view LiteralKind, NominalId Type )
        {
            const Symbol Key = Strings.Intern( LiteralKind );
            if ( const auto It = ByLiteral.find( Key ); It != ByLiteral.end() )
            {
                return It->second == Type;
            }
            ByLiteral.emplace( Key, Type );
            return true;
        }

        [[nodiscard]] std::optional<NominalId> LookupLiteral ( std::string_view LiteralKind ) const
        {
            return Find( ByLiteral, LiteralKind );
        }

        // --- Layouts -----------------------------------------------------

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

    private:

        template <typename MapType>
        [[nodiscard]] std::optional<NominalId> Find ( const MapType &Map, std::string_view Text ) const
        {
            // Lookup must never intern: a miss on an unknown name would
            // otherwise grow the table on every read-only query.
            const auto Known = Strings.Find( Text );
            if ( not Known )
            {
                return std::nullopt;
            }
            if ( const auto It = Map.find( *Known ); It != Map.end() )
            {
                return It->second;
            }
            return std::nullopt;
        }

        Core::StringInterner Strings;
        Core::Arena<NominalType, NominalId> Types;
        Core::Arena<LayoutNode, LayoutId> Layouts;
        std::unordered_map<Symbol, NominalId> ByName;
        std::unordered_map<Symbol, NominalId> ByLiteral;
    };

} // namespace Sema

} // namespace Volt
