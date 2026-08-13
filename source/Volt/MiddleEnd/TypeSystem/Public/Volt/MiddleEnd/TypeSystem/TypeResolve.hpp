#pragma once

// TypeResolve.hpp — one algorithm that turns a written type annotation into a
// resolved type, templated on the sink so it is written once and instantiated
// twice: SigSink encodes generic parameters (declarations), UnitSink refuses
// them (expressions, which are always concrete).
//
// Zero-hardcode: outside `TypeRef` there is no branch per type shape. `T*`,
// `T?`, `T[N]`, `(A) -> B` and every future sugar go through the *same* line —
// the node's reflected name is looked up in the store's node-kind table, and
// its TypeId / TypeList fields, in declaration order, become the arguments.
// The C++ never spells a node name nor a Volt type name.

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Type.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndTypeSystem_export.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Volt
{

namespace MiddleEnd
{

    namespace TypeSystem
    {

        // Encodes a declaration's annotation: a name that is a generic parameter
        // of the enclosing type becomes a parameter reference, not a nominal.
        struct SigSink
        {

            using IdType = SigTypeId;

            TypeStore &Store;
            // Only `typeof` reports through this — a signature is the one place
            // it cannot be answered, and a field left silently untyped is worse
            // than a refusal. Null keeps ResolveTypeExpr's ordinary "an unknown
            // name yields an invalid id and no diagnostic" behaviour.
            ::Volt::Core::DiagEngine::Bag *Diags = nullptr;

            [[nodiscard]] IdType Make ( NominalId Base, ::Volt::Core::SmallVec<IdType, 2> Args )
            {
                return Store.AddSig( SigType{ .Base = Base, .ParamIndex = -1, .Args = std::move( Args ) } );
            }

            [[nodiscard]] IdType Param ( std::int32_t Index )
            {
                return Store.AddSig( SigType{ .Base = NominalId{}, .ParamIndex = Index, .Args = {} } );
            }

            [[nodiscard]] IdType SelfRef ()
            {
                return Param( SigType::SelfParam );
            }

            // `typeof( expr )` in a *declaration's* signature — a field type, a
            // parameter, a return type. Unanswerable here and deliberately so:
            // a signature is published for other units to resolve against, and
            // an expression's type is a fact about one body in one unit, whose
            // AST that other unit never sees. Refused out loud, because the
            // alternative is a field whose type is silently nothing.
            [[nodiscard]] IdType TypeOf ( Frontend::ExprId /*Value*/, ::Volt::Core::SourceRange Loc ) const
            {
                if ( Diags != nullptr )
                {
                    Diags->Report( ::Volt::Core::Diagnostic{
                        .Severity = ::Volt::Core::ESeverity::Error,
                        .Range    = Loc,
                        .Message  = "'typeof' cannot be used in a declaration's type — it names the type of an "
                                    "expression, which only exists inside a body",
                        .Notes    = {} } );
                }
                return IdType{};
            }
        };

        // Encodes an expression's type. A free parameter normally cannot appear
        // here, so it yields an invalid id — the caller decides whether that is an
        // error. `self`, on the other hand, is already known inside a body, and is
        // whatever type that body belongs to.
        //
        // `Bindings` is the one case where a parameter *is* answerable: a
        // re-instantiation (MiddleEnd::TypeSystem::ReinstantiateBody) walks a generic body with its
        // arguments already fixed, so `sizeof T` inside `Pointer<T>#malloc` is a
        // concrete width there and a deferral everywhere else. Empty in an
        // ordinary pass, which is what keeps that deferral the default.
        struct UnitSink
        {

            using IdType = SemaTypeId;

            UnitTypes &Values;
            SemaTypeId Self;
            std::span<const SemaTypeId> Bindings;
            // Non-null enables generic-bound checking (MiddleEnd::TypeSystem::CheckGenericBounds)
            // at every `TypeRef` this sink resolves — nullptr keeps
            // ResolveTypeExpr's ordinary "no diagnostic" behaviour for a caller
            // that only wants a best-effort type (e.g. MiddleEnd::TypeSystem::ReinstantiateBody).
            ::Volt::Core::DiagEngine::Bag *Diags = nullptr;

            // `typeof( expr )` is the one annotation whose answer is an
            // *inference*, and inference lives in Analysis — above this module
            // in §1's DAG. Rather than invert that, the walk which owns an
            // inferencer installs itself here and `ResolveTypeExpr` calls back
            // through it; nothing in this module's headers points at Analysis.
            //
            // Null in every caller that has no walk state to lend
            // (`ReinstantiateBody`'s best-effort resolve, any pass resolving an
            // annotation outside a body), where a `typeof` has no answer and
            // yields an invalid id like any other unresolvable annotation.
            SemaTypeId ( *InferHook )( void *State, Frontend::ExprId Id ) = nullptr;
            void *InferState                                              = nullptr;

            [[nodiscard]] IdType Make ( NominalId Base, ::Volt::Core::SmallVec<IdType, 2> Args )
            {
                return Values.Intern( SemaType{ .Base = Base, .Args = std::move( Args ) } );
            }

            // No diagnostic on a miss, unlike SigSink's: a hookless UnitSink is
            // an ordinary best-effort resolve (ReinstantiateBody), not a
            // refusal, and reporting there would fire once per instantiation.
            [[nodiscard]] IdType TypeOf ( Frontend::ExprId Id, ::Volt::Core::SourceRange /*Loc*/ ) const
            {
                return InferHook != nullptr and Id.IsValid() ? InferHook( InferState, Id ) : IdType{};
            }

            [[nodiscard]] IdType Param ( std::int32_t Index )
            {
                if ( Index >= 0 and static_cast<std::size_t>( Index ) < Bindings.size() )
                {
                    return Bindings[static_cast<std::size_t>( Index )];
                }
                return IdType{};
            }

            [[nodiscard]] IdType SelfRef () const
            {
                return Self;
            }
        };

        // A generic parameter whose declared bound (`T : Bound`) the supplied
        // argument does not satisfy.
        struct BoundViolation
        {

            std::size_t ParamIndex = 0;
            NominalId RequiredBound;
            NominalId SuppliedBase;
        };

        /// `Base<Args...>`: does every bounded parameter of `Base` accept the
        /// concrete type supplied at the matching index? Mirrors
        /// `Analysis::CheckAbstractConformance`'s question but in the
        /// opposite direction — that checks a *declaration* satisfies the
        /// mixins it includes, this checks a call-site *argument* satisfies a
        /// bound the generic parameter itself declared. A bound is satisfied the
        /// same way an includer satisfies a mixin: every abstract member the
        /// bound's own body declares must resolve, through LookupMemberOn, to a
        /// non-abstract implementation.
        ///
        /// An argument that is itself unresolved (deferred — inside another
        /// generic body, `T` has no concrete value yet) is skipped rather than
        /// reported: there is nothing yet to check, and the concrete
        /// instantiation this deferred to will be checked when it, in turn,
        /// resolves a concrete argument.
        ///
        /// Returns the first violation found, in parameter order — one
        /// diagnostic per `TypeRef`, matching every other check in this layer.
        [[nodiscard]] VOLT_MIDDLEEND_TYPESYSTEM_EXPORT std::optional<BoundViolation>
        CheckGenericBounds ( const TypeStore &Store, UnitTypes &Values, NominalId Base, std::span<const SemaTypeId> Args );

        /// Resolve the type annotation `Id` through `Out`. `Generics` are the
        /// enclosing declaration's generic parameter names, matched by symbol.
        /// An unknown name yields an invalid id and *no* diagnostic.
        template <typename Sink>
        [[nodiscard]] auto ResolveTypeExpr ( const Frontend::AstContext &Ast,
                                             const TypeStore &Store,
                                             std::span<const Frontend::Symbol> Generics,
                                             Sink &Out,
                                             Frontend::TypeId Id ) -> typename Sink::IdType
        {
            using IdType = typename Sink::IdType;

            if ( not Id.IsValid() )
            {
                return IdType{};
            }

            return std::visit(
                Meta::Overloaded{
                    [&] ( const Frontend::TypeRef &Ref ) -> IdType
                    {
                        if ( Ref.Path.Size() == 0 )
                        {
                            return IdType{};
                        }
                        const Frontend::Symbol Last = Ref.Path[Ref.Path.Size() - 1];

                        // A bare name that is one of the enclosing generics is a
                        // parameter reference, not a type of its own. So is the
                        // `self` keyword — spelled from the token table, since it
                        // is syntax and not a Volt type name.
                        if ( Ref.Path.Size() == 1 )
                        {
                            if ( Ast.Text( Last ) == Frontend::TokenSpelling( Frontend::TokenKind::KwSelf ) )
                            {
                                return Out.SelfRef();
                            }

                            for ( std::size_t Index = 0; Index < Generics.size(); ++Index )
                            {
                                if ( Generics[Index] == Last )
                                {
                                    return Out.Param( static_cast<std::int32_t>( Index ) );
                                }
                            }
                        }

                        const auto Base = Store.LookupType( Ast.Text( Last ) );
                        if ( not Base )
                        {
                            return IdType{};
                        }

                        ::Volt::Core::SmallVec<IdType, 2> Args;
                        for ( const Frontend::TypeId Arg : Ref.Generics )
                        {
                            Args.PushBack( ResolveTypeExpr( Ast, Store, Generics, Out, Arg ) );
                        }

                        // Bound checking only ever applies to a sink that yields
                        // concrete, already-interned types (UnitSink) — a
                        // SigSink's Args may themselves be un-instantiated
                        // parameter references, which have nothing to check yet.
                        if constexpr ( std::is_same_v<IdType, SemaTypeId> )
                        {
                            if ( Out.Diags != nullptr )
                            {
                                const std::span<const SemaTypeId> Concrete{ Args.begin(), Args.Size() };
                                if ( const auto Violation = CheckGenericBounds( Store, Out.Values, *Base, Concrete ) )
                                {
                                    const NominalType &BaseType = Store.Type( *Base );
                                    const std::string ParamName =
                                        Violation->ParamIndex < BaseType.Params.Size()
                                            ? std::string{ Store.Text( BaseType.Params[Violation->ParamIndex] ) }
                                            : std::string{ "?" };
                                    const std::string ArgName =
                                        Violation->SuppliedBase.IsValid()
                                            ? std::string{ Store.Text( Store.Type( Violation->SuppliedBase ).Name ) }
                                            : std::string{ "<unresolved>" };
                                    const std::string BoundName =
                                        Violation->RequiredBound.IsValid()
                                            ? std::string{ Store.Text( Store.Type( Violation->RequiredBound ).Name ) }
                                            : std::string{ "<unresolved>" };
                                    Out.Diags->Report( ::Volt::Core::Diagnostic{
                                        .Severity = ::Volt::Core::ESeverity::Error,
                                        .Range    = Ref.Loc,
                                        .Message  = "type " + ArgName + " does not satisfy bound " + BoundName +
                                                   " required by generic parameter " + ParamName + " of " +
                                                   std::string{ Store.Text( BaseType.Name ) },
                                        .Notes = {} } );
                                }
                            }
                        }

                        return Out.Make( *Base, std::move( Args ) );
                    },
                    // `typeof( expr )` — the only node here built from a
                    // *value*, so the only one the reflected arm below cannot
                    // reach: it has no TypeId field to recurse on and claims no
                    // node kind. The sink answers it, or says it cannot.
                    [&] ( const Frontend::TypeOfType &Node ) -> IdType { return Out.TypeOf( Node.Value, Node.Loc ); },
                    // Every other shape, present or future: the node claims a
                    // stdlib type through `@[Literal( <NodeName> )]`, and its
                    // type-bearing fields are its arguments in declaration order.
                    [&] ( const auto &Node ) -> IdType
                    {
                        using NodeType = std::remove_cvref_t<decltype( Node )>;
                        if constexpr ( not Meta::Reflected<NodeType> )
                        {
                            return IdType{};
                        }
                        else
                        {
                            const auto Base = Store.LookupNodeKind( Meta::TypeName<NodeType>() );
                            if ( not Base )
                            {
                                return IdType{};
                            }

                            ::Volt::Core::SmallVec<IdType, 2> Args;
                            Meta::ForEachField( Node,
                                                [&] ( std::string_view, const auto &Field )
                                                {
                                                    using FieldType = std::remove_cvref_t<decltype( Field )>;
                                                    if constexpr ( std::is_same_v<FieldType, Frontend::TypeId> )
                                                    {
                                                        Args.PushBack( ResolveTypeExpr( Ast, Store, Generics, Out, Field ) );
                                                    }
                                                    else if constexpr ( std::is_same_v<FieldType, Frontend::TypeList> )
                                                    {
                                                        for ( const Frontend::TypeId Child : Field )
                                                        {
                                                            Args.PushBack( ResolveTypeExpr( Ast, Store, Generics, Out, Child ) );
                                                        }
                                                    }
                                                } );
                            return Out.Make( *Base, std::move( Args ) );
                        }
                    },
                },
                Ast.Type( Id ) );
        }

        /// Substitute a declared signature's generic parameters by the receiver's
        /// arguments and its `self` by `Self`, producing a concrete per-unit type.
        /// An out-of-range parameter (an un-instantiated generic) yields an
        /// invalid id.
        [[nodiscard]] VOLT_MIDDLEEND_TYPESYSTEM_EXPORT SemaTypeId Instantiate (
            const TypeStore &Store, SigTypeId Id, std::span<const SemaTypeId> ReceiverArgs, SemaTypeId Self, UnitTypes &Values );

        // A member found on a receiver, with the *already concrete* type that
        // declares it: looking `map` up on `Array<Int32>` yields the member as
        // declared by `Enumerable`, owned by `Enumerable<Int32>`. That owner is
        // what a signature's ParamIndex counts against, which is why the plain
        // nominal TypeStore::LookupMember returns is not enough.
        struct InstantiatedMember
        {

            const Member *Decl = nullptr;
            SemaTypeId Owner;
        };

        /// Find `Name` on `Receiver`, composing generic arguments along the way:
        /// own body, then each `include`, then the superclass, each parent first
        /// instantiated against the arguments the child supplies it.
        ///
        /// `Self` stays the *original* receiver through the whole descent, never
        /// the mixin being traversed — that is what makes `Comparable#<( other :
        /// self )` mean `Int32` when reached from an Int32.
        /// Walk a declared signature and a concrete type in parallel, binding
        /// every generic parameter the pattern still mentions. `def map<U>(
        /// &block : T -> U )` given a block of type `Proc< Bool, Int32 >` learns
        /// `U = Bool` — the only way a method generic can ever be known, since
        /// the receiver says nothing about it.
        ///
        /// A slot already bound is left alone, and a shape mismatch simply
        /// teaches nothing: inference reports no diagnostics of its own, exactly
        /// like the rest of this file, and an unresolved slot stays invalid.
        VOLT_MIDDLEEND_TYPESYSTEM_EXPORT void UnifySig ( const TypeStore &Store,
                                                         const UnitTypes &Values,
                                                         SigTypeId Pattern,
                                                         SemaTypeId Actual,
                                                         std::span<SemaTypeId> Bindings );

        [[nodiscard]] VOLT_MIDDLEEND_TYPESYSTEM_EXPORT InstantiatedMember LookupMemberOn ( const TypeStore &Store,
                                                                                           UnitTypes &Values,
                                                                                           SemaTypeId Receiver,
                                                                                           SemaTypeId Self,
                                                                                           std::string_view Name,
                                                                                           std::uint32_t Depth = 0 );

        [[nodiscard]] VOLT_MIDDLEEND_TYPESYSTEM_EXPORT bool
        IsSubclassOf ( const TypeStore &Store, NominalId Child, NominalId Parent );

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
