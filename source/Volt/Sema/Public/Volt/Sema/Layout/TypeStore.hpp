#pragma once

#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Sema/Layout/MemoryLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

        // `self` in a signature is not a type but a *deferred* one: the
        // receiver's, whatever it turns out to be. `Comparable#<( other : self )`
        // must mean `Int32` on an Int32 and `String` on a String, so it cannot
        // resolve to a nominal at declaration time any more than `T` can.
        static constexpr std::int32_t SelfParam = -2;

        NominalId Base;               // invalid when ParamIndex != -1
        std::int32_t ParamIndex = -1; // >= 0 => a generic parameter; SelfParam => `self`
        Core::SmallVec<SigTypeId, 2> Args;
    };

    enum class EMemberKind : std::uint8_t
    {

        Field,
        Method,
        // An `enum` case (`Red`, `Some( value : T )`): a named, self-typed
        // constant, reachable both as `Enum::Case` (a naked-type access) and
        // as `.Case` inside a `case self when` pattern (an instance access)
        // — CheckMemberSelf exempts this kind from the static/instance
        // split for exactly that reason.
        EnumCase,
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
        // How many of the parameter space's slots belong to the method
        // rather than to the declaring type. A signature resolves against
        // the two concatenated — type generics first, method generics after
        // — so on `Array<T>` the `U` of `def map<U>` is ParamIndex 1. The
        // receiver answers the first Params.Size() of them; these last
        // OwnGenerics are holes only the call site can fill.
        std::uint32_t OwnGenerics = 0;
        std::uint32_t MinParams   = 0;
        bool bSelf                = false; // `def self.malloc`
        // `abstract def`: a contract, not an implementation. A mixin uses
        // one to state what an including type owes it, and that debt is
        // what the conformance check collects.
        bool bAbstract = false;
        // `@[Apply]`: the member's signature *is* the receiver's type
        // arguments — result first, then parameters — rather than what it
        // wrote down. This is how a callable is invoked without the compiler
        // knowing what a callable is: the stdlib type claiming the FuncType
        // node marks its own `call`, and arity follows the receiver, which no
        // fixed declaration could express.
        bool bApply = false;
        // `@[External( "libc", "malloc" )]`: implemented outside Volt, so the
        // member is a *declaration* of a symbol the linker resolves rather
        // than a body any backend emits. Recorded here, at the one seam that
        // already reads annotations, because a backend must never re-scan an
        // AST for sibling Annotation decls — that would be semantic analysis
        // in codegen (.agents/backend/core-interfaces.md).
        //
        // ExternLib is the library to link ("libc"); ExternSymbol is the C
        // symbol, defaulting to the member's own spelling when the annotation
        // gives only a library. Both invalid when the member is ordinary Volt.
        Symbol ExternLib;
        Symbol ExternSymbol;
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
        // `class X < Y<T>` / `include Enumerable<T>`, kept *as written*, in
        // the including type's own parameter space: a bare NominalId would
        // drop the `<T>` and leave a mixin's members talking about a generic
        // nobody can answer. Instantiate() against the receiver's arguments
        // turns one of these into the concrete parent.
        SigTypeId Super;
        Core::SmallVec<SigTypeId, 2> Includes;

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
                Existing.Members.clear();
                Existing.Includes.Clear();
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

        void SetSuper ( NominalId Id, SigTypeId Super )
        {
            Types.Get( Id ).Super = Super;
        }

        void AddInclude ( NominalId Id, SigTypeId Mixin )
        {
            Types.Get( Id ).Includes.PushBack( Mixin );
        }

        void AddMember ( NominalId Id, Member Entry )
        {
            Types.Get( Id ).Members.push_back( std::move( Entry ) );
        }

        [[nodiscard]] std::optional<NominalId> LookupTypeByDecl ( std::uint32_t Unit, Frontend::DeclId Decl ) const
        {
            for ( std::size_t Index = 0; Index < Types.Size(); ++Index )
            {
                const NominalId Id{ static_cast<std::uint32_t>( Index ) };
                if ( Types.Get( Id ).Unit == Unit and Types.Get( Id ).Decl == Decl )
                {
                    return Id;
                }
            }
            return std::nullopt;
        }

        // The member declared by `Decl` inside `Id`'s own body, mutable so the
        // signature phase can fill in what the declaration phase could not yet
        // resolve. A DeclId is unique within its unit, so matching both Unit and
        // Decl is exact even across units.
        [[nodiscard]] Member *MemberByDecl ( NominalId Id, std::uint32_t Unit, Frontend::DeclId Decl )
        {
            for ( Member &Entry : Types.Get( Id ).Members )
            {
                if ( Entry.Unit == Unit and Entry.Decl == Decl )
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

        // The member `Id`'s *own* body declares, ignoring everything it
        // inherits. The one step both traversals share: this one, which
        // answers name existence, and LookupMemberOn (TypeResolve.hpp),
        // which additionally carries generic arguments down the chain.
        [[nodiscard]] const Member *OwnMember ( NominalId Id, std::string_view Name ) const
        {
            if ( not Id.IsValid() )
            {
                return nullptr;
            }

            // Lookup must never intern: a miss on an unknown name would
            // otherwise grow the table on every read-only query.
            const auto Key = Strings.Find( Name );
            if ( not Key )
            {
                return nullptr;
            }

            for ( const Member &Entry : Types.Get( Id ).Members )
            {
                if ( Entry.Name == *Key )
                {
                    return &Entry;
                }
            }
            return nullptr;
        }

        // The nominal a parent link names, dropping its arguments — enough
        // to answer "does this name exist anywhere above me", which is all
        // this traversal claims to do.
        [[nodiscard]] NominalId BaseOf ( SigTypeId Id ) const
        {
            return Id.IsValid() ? Sigs.Get( Id ).Base : NominalId{};
        }

        // Own body first, then mixins, then the superclass — transitively.
        // Depth is bounded so a malformed cyclic hierarchy cannot hang sema.
        //
        // Name existence only: the MemberRef it hands back carries the
        // declaring nominal, not an instantiation, so a signature that
        // mentions the owner's generics cannot be read off it. Typing a
        // member access goes through LookupMemberOn instead.
        [[nodiscard]] MemberRef LookupMember ( NominalId Id, std::string_view Name, std::uint32_t Depth = 0 ) const
        {
            if ( not Id.IsValid() or Depth > 16 )
            {
                return MemberRef{};
            }

            if ( const Member *Own = OwnMember( Id, Name ); Own != nullptr )
            {
                return MemberRef{ .Decl = Own, .Owner = Id };
            }

            const NominalType &Type = Types.Get( Id );
            for ( const SigTypeId Mixin : Type.Includes )
            {
                if ( const MemberRef Found = LookupMember( BaseOf( Mixin ), Name, Depth + 1 ); Found.Decl != nullptr )
                {
                    return Found;
                }
            }
            return LookupMember( BaseOf( Type.Super ), Name, Depth + 1 );
        }

        // --- Free (top-level) functions ------------------------------------

        // A `def` declared directly in a module, not inside any type — its
        // own namespace, so a free function and a type may share a spelling
        // without colliding. Modelled as a `Member` (Kind == Method) so the
        // call-checking machinery built for methods (Resolution,
        // CheckCallArgs, Reinstantiate) applies unchanged: a free function is
        // simply a method with no receiver.
        [[nodiscard]] Member *DeclareFunction ( std::string_view Name, std::uint32_t Unit, Frontend::DeclId Decl )
        {
            const Symbol Key = Strings.Intern( Name );
            if ( const auto It = FunctionByName.find( Key ); It != FunctionByName.end() )
            {
                Member &Existing = Functions[It->second];
                Existing.Unit    = Unit;
                Existing.Decl    = Decl;
                return &Existing;
            }

            Member Fresh;
            Fresh.Name              = Key;
            Fresh.Kind              = EMemberKind::Method;
            Fresh.Unit              = Unit;
            Fresh.Decl              = Decl;
            const std::size_t Index = Functions.size();
            Functions.push_back( std::move( Fresh ) );
            FunctionByName.emplace( Key, Index );
            return &Functions[Index];
        }

        // The function declared by `Decl` inside `Unit`, mutable so the
        // signature phase can fill in what the declaration phase could not
        // yet resolve — mirrors MemberByDecl.
        [[nodiscard]] Member *FunctionByDecl ( std::uint32_t Unit, Frontend::DeclId Decl )
        {
            for ( Member &Entry : Functions )
            {
                if ( Entry.Unit == Unit and Entry.Decl == Decl )
                {
                    return &Entry;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const Member *LookupFunction ( std::string_view Name ) const
        {
            const auto Known = Strings.Find( Name );
            if ( not Known )
            {
                return nullptr;
            }
            const auto It = FunctionByName.find( *Known );
            return It != FunctionByName.end() ? &Functions[It->second] : nullptr;
        }

        // --- Modules -------------------------------------------------------

        // A `module` is a namespace, not a nominal type: its methods are bound
        // as free functions (Layout/TypeBinder.cpp) and it has no layout, no
        // `self`, no members. Only its *name* is kept, and only so that
        // `MathUtils.square( 4 )` can be told apart from a member access on an
        // unresolved receiver — the first resolves to the free function, the
        // second is a genuine unknown.
        void DeclareModule ( std::string_view Name )
        {
            Modules.insert( Strings.Intern( Name ) );
        }

        [[nodiscard]] bool IsModule ( std::string_view Name ) const
        {
            const auto Known = Strings.Find( Name );
            return Known.has_value() and Modules.contains( *Known );
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

        // --- Exception root ------------------------------------------------

        // The one stdlib type annotated `@[ExceptionRoot]` (Exception.vl).
        // `raise`/`rescue` reason about it through this binding instead of
        // the C++ side ever spelling out the Volt type name "Exception".
        bool SetExceptionRoot ( NominalId Id )
        {
            if ( ExceptionRoot.IsValid() and ExceptionRoot != Id )
            {
                return false;
            }
            ExceptionRoot = Id;
            return true;
        }

        [[nodiscard]] std::optional<NominalId> GetExceptionRoot () const
        {
            return ExceptionRoot.IsValid() ? std::optional<NominalId>{ ExceptionRoot } : std::nullopt;
        }

        // --- Layouts -----------------------------------------------------

        [[nodiscard]] LayoutId AddPrimitive ( Symbol Spelling, std::uint32_t Bits )
        {
            return Layouts.Add( LayoutNode{ Primitive{ .Spelling = Spelling, .Bits = Bits } } );
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
        NominalId ExceptionRoot;
        // Free functions: not owned by any NominalId, so a plain vector +
        // name index rather than the Members arrays. Never reallocated once
        // the serial TypeBinder seam ends, so the raw Member* handed out by
        // Resolution (TypeCheckerContext.hpp) stays valid through every
        // (parallel, read-only) TypeChecker run.
        std::vector<Member> Functions;
        std::unordered_map<Symbol, std::size_t> FunctionByName;
        std::unordered_set<Symbol> Modules;
    };

} // namespace Sema

} // namespace Volt
