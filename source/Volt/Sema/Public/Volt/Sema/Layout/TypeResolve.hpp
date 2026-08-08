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

#include "Sema_export.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Type.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Volt
{

namespace Sema
{

    // Encodes a declaration's annotation: a name that is a generic parameter
    // of the enclosing type becomes a parameter reference, not a nominal.
    struct SigSink
    {

        using IdType = SigTypeId;

        TypeStore &Store;

        [[nodiscard]] IdType Make ( NominalId Base, Core::SmallVec<IdType, 2> Args )
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
    };

    // Encodes an expression's type. A free parameter normally cannot appear
    // here, so it yields an invalid id — the caller decides whether that is an
    // error. `self`, on the other hand, is already known inside a body, and is
    // whatever type that body belongs to.
    //
    // `Bindings` is the one case where a parameter *is* answerable: a
    // re-instantiation (Sema::ReinstantiateBody) walks a generic body with its
    // arguments already fixed, so `sizeof T` inside `Pointer<T>#malloc` is a
    // concrete width there and a deferral everywhere else. Empty in an
    // ordinary pass, which is what keeps that deferral the default.
    struct UnitSink
    {

        using IdType = SemaTypeId;

        UnitTypes &Values;
        SemaTypeId Self;
        std::span<const SemaTypeId> Bindings;
        // Non-null enables generic-bound checking (Sema::CheckGenericBounds)
        // at every `TypeRef` this sink resolves — nullptr keeps
        // ResolveTypeExpr's ordinary "no diagnostic" behaviour for a caller
        // that only wants a best-effort type (e.g. Sema::ReinstantiateBody).
        Core::DiagEngine::Bag *Diags = nullptr;

        [[nodiscard]] IdType Make ( NominalId Base, Core::SmallVec<IdType, 2> Args )
        {
            return Values.Intern( SemaType{ .Base = Base, .Args = std::move( Args ) } );
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
    /// `TypeCheckerPass::CheckAbstractConformance`'s question but in the
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
    [[nodiscard]] SEMA_EXPORT std::optional<BoundViolation> CheckGenericBounds (
        const TypeStore &Store, UnitTypes &Values, NominalId Base, std::span<const SemaTypeId> Args );

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

                    Core::SmallVec<IdType, 2> Args;
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
                                const std::string ArgName = Violation->SuppliedBase.IsValid()
                                                                 ? std::string{ Store.Text( Store.Type( Violation->SuppliedBase ).Name ) }
                                                                 : std::string{ "<unresolved>" };
                                const std::string BoundName = Violation->RequiredBound.IsValid()
                                                                   ? std::string{ Store.Text( Store.Type( Violation->RequiredBound ).Name ) }
                                                                   : std::string{ "<unresolved>" };
                                Out.Diags->Report( Core::Diagnostic{
                                    .Severity = Core::ESeverity::Error,
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

                        Core::SmallVec<IdType, 2> Args;
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
    [[nodiscard]] SEMA_EXPORT SemaTypeId Instantiate (
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
    SEMA_EXPORT void UnifySig (
        const TypeStore &Store, const UnitTypes &Values, SigTypeId Pattern, SemaTypeId Actual, std::span<SemaTypeId> Bindings );

    [[nodiscard]] SEMA_EXPORT InstantiatedMember LookupMemberOn ( const TypeStore &Store,
                                                                  UnitTypes &Values,
                                                                  SemaTypeId Receiver,
                                                                  SemaTypeId Self,
                                                                  std::string_view Name,
                                                                  std::uint32_t Depth = 0 );

    [[nodiscard]] SEMA_EXPORT bool IsSubclassOf ( const TypeStore &Store, NominalId Child, NominalId Parent );

} // namespace Sema

} // namespace Volt
