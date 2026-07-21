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
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Type.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstdint>
#include <span>
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
    };

    // Encodes an expression's type. A free parameter cannot appear here, so
    // it yields an invalid id — the caller decides whether that is an error.
    struct UnitSink
    {

        using IdType = SemaTypeId;

        UnitTypes &Values;

        [[nodiscard]] IdType Make ( NominalId Base, Core::SmallVec<IdType, 2> Args )
        {
            return Values.Intern( SemaType{ .Base = Base, .Args = std::move( Args ) } );
        }

        [[nodiscard]] IdType Param ( std::int32_t )
        {
            return IdType{};
        }
    };

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
                    // parameter reference, not a type of its own.
                    if ( Ref.Path.Size() == 1 )
                    {
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
    /// arguments, producing a concrete per-unit type. An out-of-range
    /// parameter (an un-instantiated generic) yields an invalid id.
    [[nodiscard]] SEMA_EXPORT SemaTypeId Instantiate ( const TypeStore &Store,
                                                       SigTypeId Id,
                                                       std::span<const SemaTypeId> ReceiverArgs,
                                                       UnitTypes &Values );

} // namespace Sema

} // namespace Volt
