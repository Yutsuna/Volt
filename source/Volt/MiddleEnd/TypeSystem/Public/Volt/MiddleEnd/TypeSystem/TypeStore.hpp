#pragma once

#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/TypeSystem/MemoryLayout.hpp"
#include "VoltMiddleEndTypeSystem_export.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Volt
{

namespace Meta
{
    class Writer;
    class Reader;
} // namespace Meta

namespace MiddleEnd
{

    namespace TypeSystem
    {

        // Structural keys — a flat `u32` sequence — are how both dedup tables in
        // this tier are hashed: `TypeStore::AddSig` for signature types, and
        // `TypeUniverse`/`UnitTypes` for expression types. One encoding, one
        // hash, so the two can never disagree about what "the same shape" means.
        struct U32KeyHash
        {

            [[nodiscard]] std::size_t operator()( const std::vector<std::uint32_t> &Key ) const noexcept
            {
                // FNV-1a over the words: cheap, and these keys are 1-4 words in
                // the overwhelming majority of cases.
                std::size_t Hash = 1469598103934665603ULL;
                for ( const std::uint32_t Word : Key )
                {
                    Hash ^= Word;
                    Hash *= 1099511628211ULL;
                }
                return Hash;
            }
        };

        // Defined in TypeUniverse.hpp, which needs `NominalId` from this header
        // and so cannot be included here. The store owns one by pointer; see
        // `TypeStore::Universe`.
        class TypeUniverse;

        // A declared Volt type, referenced by handle like everything else.
        struct NominalTag
        {
        };

        using NominalId = ::Volt::Core::TypedId<NominalTag>;

        // A type *as declared in a signature*. Unlike an expression's type it may
        // still reference a generic parameter of the type that carries it:
        // `def push( value : T )` on `Array<T>` stores ParamIndex = 0, not a
        // NominalId, because there is no such thing as "the" T. Instantiate()
        // turns one of these into a concrete per-unit type once a receiver
        // supplies the arguments.
        struct SigTypeTag
        {
        };

        using SigTypeId = ::Volt::Core::TypedId<SigTypeTag>;

        struct SigType
        {

            // `self` in a signature is not a type but a *deferred* one: the
            // receiver's, whatever it turns out to be. `Comparable#<( other : self )`
            // must mean `Int32` on an Int32 and `String` on a String, so it cannot
            // resolve to a nominal at declaration time any more than `T` can.
            static constexpr std::int32_t SelfParam = -2;

            NominalId Base;               // invalid when ParamIndex != -1
            std::int32_t ParamIndex = -1; // >= 0 => a generic parameter; SelfParam => `self`
            ::Volt::Core::SmallVec<SigTypeId, 2> Args;
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
            std::uint32_t Unit = 0;                      // ordinal of the declaring unit
            Frontend::DeclId Decl;                       // its declaration, inside `Unit`
            SigTypeId Result;                            // field type / method return type
            ::Volt::Core::SmallVec<SigTypeId, 4> Params; // methods only
            // Parallel to Params: whether that slot is a `&block` parameter —
            // it binds through a call's trailing `do ... end` / BlockArg, never
            // through the positional argument list, so callers matching Params
            // against Args must skip it.
            ::Volt::Core::SmallVec<bool, 4> ParamIsBlock;
            // Parallel to Params: does this method keep the argument handed to
            // that slot past the call — store it in a field, hand it to a
            // constructor, return it — rather than merely reading it?
            //
            // Derived by `Raii::InferParameterEscape`, never annotated, at the
            // same serial Driver seam and for the same reason `bReturnsOwned` is
            // (see its own comment below). It answers the one question a caller's
            // full-expression region cannot answer for itself: whether releasing
            // an owned temporary after the call frees a buffer the callee is
            // still pointing at. `arr.push( "x".dup )` is exactly that shape, and
            // without this bit it is a double free rather than a leak.
            //
            // `true` is the safe default and the fixpoint's initial value: an
            // unanalysable callee (external, abstract, a cache-hit slot with no
            // AST) leaves its arguments unreleased, which is a counted leak. A
            // wrong `false` would be corruption, so nothing sets one without a
            // proof. Empty means "not yet sized" and reads as `true` everywhere —
            // see `Raii::ParameterEscapes`, the single reader.
            ::Volt::Core::SmallVec<bool, 4> ParamEscapes;
            // How many of the parameter space's slots belong to the method
            // rather than to the declaring type. A signature resolves against
            // the two concatenated — type generics first, method generics after
            // — so on `Array<T>` the `U` of `def map<U>` is ParamIndex 1. The
            // receiver answers the first Params.Size() of them; these last
            // OwnGenerics are holes only the call site can fill.
            std::uint32_t OwnGenerics = 0;
            std::uint32_t MinParams   = 0;
            bool bSelf                = false; // `def self.malloc`
            // `private` / `protected` as written on the declaration, `None`
            // when the source wrote nothing — which reads as public.
            //
            // It belongs here rather than being re-read off the AST at each
            // access for the same reason `bAbstract` does: a member is
            // resolved inside whichever unit *calls* it, and that unit never
            // sees the declaring unit's AstContext. `Analysis::CheckMemberAccess`
            // is the one reader.
            Frontend::EVisibility Visibility = Frontend::EVisibility::None;
            // `abstract def`: a contract, not an implementation. A mixin uses
            // one to state what an including type owes it, and that debt is
            // what the conformance check collects.
            bool bAbstract = false;
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
            // `EnumCase` only: the case's ordinal/explicit value, decoded once
            // here (Phase A) from the already-materialized `IntLiteral`
            // (`Frontend::EnumSynthesizeOrdinals`, which runs before `TypeBinder`
            // for exactly this reason). Lets a later Sema-side lowering
            // (`Optional::Some`/`Color::Red` construction) build a fresh
            // `IntLiteral` in its own unit's arena without ever reaching across
            // into the *declaring* unit's AST, which a cross-unit `Member` may
            // not share with the unit doing the construction.
            std::int64_t EnumOrdinal = 0;
            // Does calling this member hand the caller a value the caller now
            // *owns* — a fresh aggregate nobody else will finalize — rather than
            // a view onto something the receiver still holds?
            //
            // Derived, never annotated: `Raii::InferReturnOwnership` computes it
            // by a monotone fixpoint over every declared body at the serial
            // Driver seam, and records it here for exactly the reason
            // `bConstructs`/`bIndirect` are recorded on a resolution — the
            // decision is taken once, where the information is, and every later
            // consumer reads rather than re-derives it
            // (rules/zero-hardcode.md's "record it on the resolution"). A fourth
            // annotation was never an option; the closed list stays closed.
            //
            // It lives on `Member` rather than on `CalleeEntry` because it is a
            // property of the *callee's declaration*, not of any one call site,
            // and because `Member` is the only RAII-relevant record that crosses
            // a unit boundary: `String#+` is resolved inside whichever unit calls
            // it, whose `TypeChecker` never sees the stdlib's AST.
            //
            // `false` is the safe default and the fixpoint's initial value, so an
            // unprovable case (a recursive cycle, an unresolvable callee) leaves
            // the result `Borrowed` — a measured leak, never a double free.
            bool bReturnsOwned = false;
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
            ::Volt::Core::SmallVec<Symbol, 2> Params;
            // Parallel to Params: `T : Bound` kept as written, in this type's own
            // parameter space (the same shape Super/Includes already use for a
            // parent link) — invalid when the parameter is unbounded. Resolved in
            // Phase B (a bound's own name may live in another unit), read by
            // MiddleEnd::TypeSystem::CheckGenericBounds at every concrete instantiation.
            ::Volt::Core::SmallVec<SigTypeId, 2> ParamBounds;
            // The declared interface: fields and methods of the body itself.
            std::vector<Member> Members;
            // `class X < Y<T>` / `include Enumerable<T>`, kept *as written*, in
            // the including type's own parameter space: a bare NominalId would
            // drop the `<T>` and leave a mixin's members talking about a generic
            // nobody can answer. Instantiate() against the receiver's arguments
            // turns one of these into the concrete parent.
            SigTypeId Super;
            ::Volt::Core::SmallVec<SigTypeId, 2> Includes;

            // Does destroying a value of this type do nothing at all?
            //
            // `finalize` is universal: every declared type has one, synthesized
            // empty when the source does not write it, exactly as C++ gives every
            // class a defaulted destructor. That makes `x.finalize` resolvable on
            // *anything*, which is what lets a container release its elements in
            // ordinary Volt (`Array<T>` loops and calls it) instead of having the
            // middle-end synthesize a `.size`/`[]` loop it could only build by
            // knowing what a sequence is.
            //
            // Universality costs nothing only because of this bit, the exact
            // counterpart of `std::is_trivially_destructible`: a defaulted
            // `finalize` with no field to cascade and no ancestor that writes one
            // does nothing, so no region has to inject a call to it. Set by
            // `SynthesizeFinalizeStubs` at the serial seam, read by
            // `Raii::IsFinalizeCandidateNominal` — which is now the *only*
            // question the RAII sweeps ask about a type.
            bool bTrivialFinalize = true;
            bool bMixin           = false;

            // Per generic parameter, the AST field names feeding it when a node
            // kind is lowered to this type. Empty = the default convention (the
            // node's expression-bearing fields, in declaration order). Filled
            // from the extra arguments of `@[Literal( Kind, Field, ... )]`, so a
            // future sugar node costs zero C++.
            std::vector<::Volt::Core::SmallVec<Symbol, 2>> LiteralSlots;

            // Pre-computed virtual method symbols: Slot 0 is reserved for finalize.
            // Slots 1..N correspond to the Symbols stored here.
            // Computed once in TypeBinder.cpp Phase B (zero allocation during lookup).
            ::Volt::Core::SmallVec<Symbol, 8> VTableSlots;
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
        class VOLT_MIDDLEEND_TYPESYSTEM_EXPORT TypeStore
        {

        public:

            // Out-of-line: the store owns a `TypeUniverse` by pointer, and that
            // type is only complete in TypeUniverse.cpp (its own header needs
            // `NominalId` from this one).
            TypeStore ();
            TypeStore ( const TypeStore & )           = delete;
            TypeStore ( TypeStore && )                = delete;
            TypeStore &operator=( const TypeStore & ) = delete;
            TypeStore &operator=( TypeStore && )      = delete;
            ~TypeStore ();

            // The canonical dictionary for every expression type built against
            // *this* store's nominals. Logically const — interning an
            // instantiation does not change what the store declares — which is
            // why a read-only `PassContext::Types` can still hand a parallel
            // TypeChecker somewhere to intern into. See TypeUniverse.hpp on why
            // the scope is one store and never one process.
            [[nodiscard]] TypeUniverse &Universe () const
            {
                return *UniverseStorage;
            }

            // Distinguishes this store instance from every other one the process
            // ever builds, so a cache keyed on ids minted here (see
            // `Analysis::InstantiationCache`) can refuse a key from a different
            // build instead of silently reading a stranger's nominals.
            [[nodiscard]] std::uint64_t Generation () const
            {
                return Gen;
            }

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

            void SetParams ( NominalId Id, ::Volt::Core::SmallVec<Symbol, 2> Names )
            {
                Types.Get( Id ).Params = std::move( Names );
            }

            void SetParamBounds ( NominalId Id, ::Volt::Core::SmallVec<SigTypeId, 2> Bounds )
            {
                Types.Get( Id ).ParamBounds = std::move( Bounds );
            }

            void SetLiteralSlots ( NominalId Id, std::vector<::Volt::Core::SmallVec<Symbol, 2>> Slots )
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

            void SetIsMixin ( NominalId Id, bool bMixin )
            {
                Types.Get( Id ).bMixin = bMixin;
            }

            [[nodiscard]] bool IsMixin ( NominalId Id ) const
            {
                return Id.IsValid() and Types.Get( Id ).bMixin;
            }

            [[nodiscard]] bool HasAbstractMembers ( NominalId Nominal ) const
            {
                if ( not Nominal.IsValid() )
                {
                    return false;
                }
                const auto &Members = Type( Nominal ).Members;
                return std::ranges::any_of( Members, [] ( const Member &Entry )
                                            { return Entry.Kind == EMemberKind::Method and Entry.bAbstract; } );
            }

            void SetVTableSlots ( NominalId Id, ::Volt::Core::SmallVec<Symbol, 8> Slots )
            {
                Types.Get( Id ).VTableSlots = std::move( Slots );
            }

            [[nodiscard]] std::span<const Symbol> VTableSlotsOf ( NominalId Id ) const
            {
                return Id.IsValid()
                           ? std::span<const Symbol>{ Types.Get( Id ).VTableSlots.begin(), Types.Get( Id ).VTableSlots.Size() }
                           : std::span<const Symbol>{};
            }

            [[nodiscard]] std::uint32_t VTableSlotOf ( NominalId Trait, Symbol MethodName ) const
            {
                if ( not Trait.IsValid() )
                {
                    return 0;
                }
                const auto &Slots = Types.Get( Trait ).VTableSlots;
                for ( std::size_t Index = 0; Index < Slots.Size(); ++Index )
                {
                    if ( Slots[Index] == MethodName )
                    {
                        return static_cast<std::uint32_t>( Index + 1 );
                    }
                }
                return 0;
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
            // Every member `Id` declares, mutable — the seam-time counterpart of
            // `Type( Id ).Members`. Same contract as `MemberByDecl` just below:
            // the store is only writable *before* it freezes for the parallel
            // sema phase, and both exist so a serial seam pass can fill in a fact
            // the declaration phase could not yet know (`Raii::
            // InferReturnOwnership`'s fixpoint is the second such pass).
            [[nodiscard]] std::span<Member> MutableMembers ( NominalId Id )
            {
                return Types.Get( Id ).Members;
            }

            // The whole record, mutable, under the identical contract: a serial
            // seam pass settling a fact the declaration phase could not know.
            // `SynthesizeFinalizeStubs` uses it for `bTrivialFinalize`.
            [[nodiscard]] NominalType &MutableType ( NominalId Id )
            {
                return Types.Get( Id );
            }

            // Does destroying a value of `Id` do nothing at all — the answer
            // `trivially_destructible? T` materialises. See
            // `NominalType::bTrivialFinalize`.
            //
            // An unresolved nominal answers *no*: a caller that cannot see the
            // type must run the destructor rather than skip it, which costs time
            // and never correctness. That is the same direction every other
            // unprovable case in the ownership model takes.
            [[nodiscard]] bool IsTriviallyDestructible ( NominalId Id ) const
            {
                return Id.IsValid() and Types.Get( Id ).bTrivialFinalize;
            }

            // The same, for the free functions a `module` (or a bare top-level
            // `def`) binds — they are members too, and a fixpoint over call
            // targets must reach them or `MathUtils.build_name( … )` resolves to
            // nothing at all.
            [[nodiscard]] std::span<Member> MutableFreeFunctions ()
            {
                return Functions;
            }

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
            // Mixins are walked *last-included-first*: `include Greeter` then
            // `include FormalGreeter` puts FormalGreeter nearer, so it is the
            // one whose `greet` answers. That is the only order under which
            // adding an `include` to the bottom of a body can shadow what is
            // already there — the reason a body lists them in the first place
            // — and it is what Ruby's ancestor chain does. LookupMemberOn
            // reverses the same way; the two must agree or a name would exist
            // on one path and type on the other.
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
                for ( std::size_t Slot = Type.Includes.Size(); Slot > 0; --Slot )
                {
                    if ( const MemberRef Found = LookupMember( BaseOf( Type.Includes[Slot - 1] ), Name, Depth + 1 );
                         Found.Decl != nullptr )
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

            // Every free function of the build, in declaration order. Lookup by
            // name answers "is this call resolvable"; codegen asks the opposite
            // question — "what must I emit a symbol for" — and that one needs the
            // whole set, so it is exposed rather than re-derived from the ASTs.
            [[nodiscard]] std::span<const Member> FreeFunctions () const
            {
                return Functions;
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

            // Structurally interned, exactly like an expression type in the
            // universe: a signature is immutable once added (nothing hands out a
            // mutable `SigType`), so two identical shapes are the same shape.
            // Written `T` appears once per signature that mentions it across the
            // whole stdlib, so this collapses a large fraction of the arena and
            // makes `SigTypeId` equality mean something.
            //
            // Unsynchronised on purpose: every caller sits on the serial
            // TypeBinder seam — a parallel pass only ever holds a
            // `const TypeStore &`, which cannot reach this.
            [[nodiscard]] SigTypeId AddSig ( SigType Value )
            {
                std::vector<std::uint32_t> Key = SigKeyOf( Value );
                if ( const auto It = SigDedup.find( Key ); It != SigDedup.end() )
                {
                    return It->second;
                }
                const SigTypeId Id = Sigs.Add( std::move( Value ) );
                SigDedup.emplace( std::move( Key ), Id );
                return Id;
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

            [[nodiscard]] bool IsNodeKind ( std::string_view NodeKind, NominalId Id ) const
            {
                if ( not Id.IsValid() )
                {
                    return false;
                }
                const auto Found = LookupNodeKind( NodeKind );
                return Found.has_value() and *Found == Id;
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

            // --- Frontend cache (Issue #61) -----------------------------------

            // The two-phase fixup key for a CalleeEntry/CalleeMap.hpp raw
            // pointer: a Member is uniquely identified across the whole build by
            // (declaring unit, its own DeclId), whether it lives on a NominalType
            // or in the free-function list — cheaper than tracking an owning
            // NominalId alongside every pointer just to re-derive what the
            // pointee already carries.
            [[nodiscard]] const Member *FindMemberByUnitDecl ( std::uint32_t Unit, Frontend::DeclId Decl ) const
            {
                for ( std::size_t Index = 0; Index < Types.Size(); ++Index )
                {
                    const NominalId Id{ static_cast<std::uint32_t>( Index ) };
                    for ( const Member &Entry : Types.Get( Id ).Members )
                    {
                        if ( Entry.Unit == Unit and Entry.Decl == Decl )
                        {
                            return &Entry;
                        }
                    }
                }
                for ( const Member &Entry : Functions )
                {
                    if ( Entry.Unit == Unit and Entry.Decl == Decl )
                    {
                        return &Entry;
                    }
                }
                return nullptr;
            }

            // Serialises the whole store: both arenas, both name indexes,
            // Functions + its index, Modules and the store's own
            // (private) StringInterner. A hand-written pair rather than a
            // Meta::Reflected fallback because TypeStore is a class with private
            // state and derived indexes, exactly like ::Volt::Core::Arena/StringInterner
            // themselves (rules/meta-first.md still applies inside the body: it
            // is nothing but calls to the generic Serialize/SerializeArena/
            // SerializeInterner primitives). Defined out-of-line
            // (Private/Layout/TypeStoreSerialize.cpp) since Serialize.hpp is a
            // sizeable include this header need not impose on every consumer.
            void SerializeCache ( Meta::Writer &W ) const;

            // Expects a *fresh* TypeStore (same contract as DeserializeArena/
            // DeserializeInterner): replays everything in original order so
            // NominalId/SigTypeId/LayoutId/Symbol values come back identical.
            [[nodiscard]] bool DeserializeCache ( Meta::Reader &R );

            // In-place reset to the just-constructed (empty) state. Not
            // expressible as `*this = TypeStore{}` — the embedded StringInterner
            // is neither copyable nor movable (see StringInterner::Clear), so
            // TypeStore inherits that and has no assignment operator either.
            // Needed by the frontend-cache recovery path: a failed
            // DeserializeCache may leave the store partway through a replay.
            void Clear ()
            {
                Strings.Clear();
                Types   = ::Volt::Core::Arena<NominalType, NominalId>{};
                Sigs    = ::Volt::Core::Arena<SigType, SigTypeId>{};
                Layouts = ::Volt::Core::Arena<LayoutNode, LayoutId>{};
                ByName.clear();
                ByNodeKind.clear();
                Functions.clear();
                FunctionByName.clear();
                Modules.clear();
                SigDedup.clear();
                ClearUniverse();
            }

        private:

            // Out-of-line for the same reason the destructor is: `TypeUniverse`
            // is incomplete here.
            void ClearUniverse ();

            // Rebuilt after a cache replay rather than persisted: it is a pure
            // index over `Sigs`, whose entries `AddSig` already guarantees are
            // duplicate-free, so it is recomputable and redundant on disk — the
            // same argument `UnitTypes` makes about its own memo.
            void RebuildSigIndex ();

            // The `AddSig` dedup encoding. `ParamIndex` is signed and its
            // sentinels are negative, so it is folded in bit-for-bit rather
            // than cast; `Base` is invalid exactly when `ParamIndex` is not, so
            // the pair never collides across the two shapes.
            [[nodiscard]] static std::vector<std::uint32_t> SigKeyOf ( const SigType &Value )
            {
                std::vector<std::uint32_t> Key;
                Key.reserve( 2 + Value.Args.Size() );
                Key.push_back( Value.Base.Value );
                Key.push_back( static_cast<std::uint32_t>( Value.ParamIndex ) );
                for ( const SigTypeId Arg : Value.Args )
                {
                    Key.push_back( Arg.Value );
                }
                return Key;
            }

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

            ::Volt::Core::StringInterner Strings;
            ::Volt::Core::Arena<NominalType, NominalId> Types;
            ::Volt::Core::Arena<SigType, SigTypeId> Sigs;
            ::Volt::Core::Arena<LayoutNode, LayoutId> Layouts;
            std::unordered_map<Symbol, NominalId> ByName;
            std::unordered_map<Symbol, NominalId> ByNodeKind;
            // Free functions: not owned by any NominalId, so a plain vector +
            // name index rather than the Members arrays. Never reallocated once
            // the serial TypeBinder seam ends, so the raw Member* handed out by
            // Resolution (TypeCheckerContext.hpp) stays valid through every
            // (parallel, read-only) TypeChecker run.
            std::vector<Member> Functions;
            std::unordered_map<Symbol, std::size_t> FunctionByName;
            std::unordered_set<Symbol> Modules;
            std::unordered_map<std::vector<std::uint32_t>, SigTypeId, U32KeyHash> SigDedup;
            std::unique_ptr<TypeUniverse> UniverseStorage;
            std::uint64_t Gen = 0;
        };

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
