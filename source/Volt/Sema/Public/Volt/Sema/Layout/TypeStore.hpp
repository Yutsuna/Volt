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
#include <vector>

namespace Volt
{

namespace Sema
{

    // A declared Volt type, referenced by handle like everything else.
    struct NominalTag
    {
    };

    using NominalId = Core::TypedId<NominalTag>;

    // A type *as declared in a signature*. Unlike an expression's type it may
    // still reference a generic parameter of the type that carries it:
    // `def push( value : T )` on `Array<T>` stores ParamIndex = 0, not a
    // NominalId, because there is no such thing as "the" T. Instantiate()
    // turns one of these into a concrete per-unit type once a receiver
    // supplies the arguments.
    struct SigTypeTag
    {
    };

    using SigTypeId = Core::TypedId<SigTypeTag>;

    struct SigType
    {

        NominalId Base;               // invalid when ParamIndex >= 0
        std::int32_t ParamIndex = -1; // >= 0 => reference to a generic parameter
        Core::SmallVec<SigTypeId, 2> Args;
    };

    enum class EMemberKind : std::uint8_t
    {

        Field,
        Method,
    };

    // One entry of a type's interface, published by the serial binder so that
    // a parallel pass in *another* unit can resolve `x.foo` without ever
    // touching that unit's AstContext — a DeclId is only meaningful inside
    // the arena that minted it, so the store keeps the resolved signature.
    struct Member
    {

        Symbol Name; // interned in the store
        EMemberKind Kind   = EMemberKind::Field;
        std::uint32_t Unit = 0;              // ordinal of the declaring unit
        Frontend::DeclId Decl;               // its declaration, inside `Unit`
        SigTypeId Result;                    // field type / method return type
        Core::SmallVec<SigTypeId, 4> Params; // methods only
        // Parallel to Params: whether that slot is a `&block` parameter —
        // it binds through a call's trailing `do ... end` / BlockArg, never
        // through the positional argument list, so callers matching Params
        // against Args must skip it.
        Core::SmallVec<bool, 4> ParamIsBlock;
        bool bSelf = false; // `def self.malloc`
    };

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

        // Generic parameter names in declaration order; a SigType's
        // ParamIndex indexes into this.
        Core::SmallVec<Symbol, 2> Params;
        // The declared interface: fields and methods of the body itself.
        std::vector<Member> Members;
        NominalId Super;                       // resolved `class X < Y`
        Core::SmallVec<NominalId, 2> Includes; // resolved mixins

        // Per generic parameter, the AST field names feeding it when a node
        // kind is lowered to this type. Empty = the default convention (the
        // node's expression-bearing fields, in declaration order). Filled
        // from the extra arguments of `@[Literal( Kind, Field, ... )]`, so a
        // future sugar node costs zero C++.
        std::vector<Core::SmallVec<Symbol, 2>> LiteralSlots;
    };

    // Owns every declared type, the layouts they resolve to, and the
    // node-kind bindings that give an untyped `10` a type. Deliberately
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

            NominalType Fresh;
            Fresh.Name         = Key;
            Fresh.Unit         = Unit;
            Fresh.Decl         = Decl;
            const NominalId Id = Types.Add( std::move( Fresh ) );
            ByName.emplace( Key, Id );
            return Id;
        }

        // --- Interface, filled by the serial binder ------------------------

        void SetParams ( NominalId Id, Core::SmallVec<Symbol, 2> Names )
        {
            Types.Get( Id ).Params = std::move( Names );
        }

        void SetLiteralSlots ( NominalId Id, std::vector<Core::SmallVec<Symbol, 2>> Slots )
        {
            Types.Get( Id ).LiteralSlots = std::move( Slots );
        }

        void SetSuper ( NominalId Id, NominalId Super )
        {
            Types.Get( Id ).Super = Super;
        }

        void AddInclude ( NominalId Id, NominalId Mixin )
        {
            Types.Get( Id ).Includes.PushBack( Mixin );
        }

        void AddMember ( NominalId Id, Member Entry )
        {
            Types.Get( Id ).Members.push_back( std::move( Entry ) );
        }

        // The member declared by `Decl` inside `Id`'s own body, mutable so the
        // signature phase can fill in what the declaration phase could not yet
        // resolve. A DeclId is unique within its unit, so this is exact even
        // for overloaded names.
        [[nodiscard]] Member *MemberByDecl ( NominalId Id, Frontend::DeclId Decl )
        {
            for ( Member &Entry : Types.Get( Id ).Members )
            {
                if ( Entry.Decl == Decl )
                {
                    return &Entry;
                }
            }
            return nullptr;
        }

        // A member found on a type, together with the type that actually
        // declares it — an inherited signature's ParamIndex counts against
        // the *owner*'s parameters, not the receiver's.
        struct MemberRef
        {

            const Member *Decl = nullptr;
            NominalId Owner;
        };

        // Own body first, then mixins, then the superclass — transitively.
        // Depth is bounded so a malformed cyclic hierarchy cannot hang sema.
        [[nodiscard]] MemberRef LookupMember ( NominalId Id, std::string_view Name, std::uint32_t Depth = 0 ) const
        {
            if ( not Id.IsValid() or Depth > 16 )
            {
                return MemberRef{};
            }

            const auto Key = Strings.Find( Name );
            if ( not Key )
            {
                return MemberRef{};
            }

            const NominalType &Type = Types.Get( Id );
            for ( const Member &Entry : Type.Members )
            {
                if ( Entry.Name == *Key )
                {
                    return MemberRef{ .Decl = &Entry, .Owner = Id };
                }
            }
            for ( const NominalId Mixin : Type.Includes )
            {
                if ( const MemberRef Found = LookupMember( Mixin, Name, Depth + 1 ); Found.Decl != nullptr )
                {
                    return Found;
                }
            }
            return LookupMember( Type.Super, Name, Depth + 1 );
        }

        // --- Signature types ----------------------------------------------

        [[nodiscard]] SigTypeId AddSig ( SigType Value )
        {
            return Sigs.Add( std::move( Value ) );
        }

        [[nodiscard]] const SigType &Sig ( SigTypeId Id ) const
        {
            return Sigs.Get( Id );
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

        // --- Node kinds --------------------------------------------------

        // Bind an *AST node name* ("IntLiteral", "ArrayLit", "PointerType",
        // ...) to the type that wraps it. `10` is an Int32 because Int32 —
        // and only Int32 — carries `@[Literal( IntLiteral )]`. The C++ side
        // never learns what "Int32" means, only that this type claimed that
        // node kind.
        //
        // The key is whatever `Frontend::NodeName()` reflects off the node,
        // so a new node in Nodes.inl becomes typeable with one `@[Literal]`
        // in the stdlib and no C++ change at all. This is also how type
        // sugar binds: `T*` is a PointerType node, and the stdlib's
        // `Pointer<T>` claims it.
        //
        // Returns false when the kind was already claimed by a *different*
        // type, so the binder can report the ambiguity instead of letting
        // stdlib file order silently decide the type of every integer.
        bool BindNodeKind ( std::string_view NodeKind, NominalId Type )
        {
            const Symbol Key = Strings.Intern( NodeKind );
            if ( const auto It = ByNodeKind.find( Key ); It != ByNodeKind.end() )
            {
                return It->second == Type;
            }
            ByNodeKind.emplace( Key, Type );
            return true;
        }

        [[nodiscard]] std::optional<NominalId> LookupNodeKind ( std::string_view NodeKind ) const
        {
            return Find( ByNodeKind, NodeKind );
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
        Core::Arena<SigType, SigTypeId> Sigs;
        Core::Arena<LayoutNode, LayoutId> Layouts;
        std::unordered_map<Symbol, NominalId> ByName;
        std::unordered_map<Symbol, NominalId> ByNodeKind;
    };

} // namespace Sema

} // namespace Volt
