#pragma once

#include "Volt/Frontend/AST/Node.hpp"

#include <cstddef>
#include <string_view>
#include <variant>

namespace Volt
{

namespace Frontend
{

    // A named type, possibly qualified and/or generic: `Int32`,
    // `Core::AppConfig`, `Array<T>`, `Pointer<T>`, `Void`.
    struct TypeRef
    {

        Core::SourceRange Loc;
        SymbolList Path;
        TypeList Generics;
    };

    // `T*` — pointer to Pointee.
    struct PointerType
    {

        Core::SourceRange Loc;
        TypeId Pointee;
    };

    // `T?` — nilable Inner.
    struct NilableType
    {

        Core::SourceRange Loc;
        TypeId Inner;
    };

    // `T[N]` — fixed-size array (Size is a constant expression).
    struct FixedArrayType
    {

        Core::SourceRange Loc;
        TypeId Elem;
        ExprId Size;
    };

    // `(A, B) -> R` / `-> R` — function type.
    //
    // Return precedes Params despite the surface syntax putting it last: the
    // reflected field order *is* the generic-argument order of whichever
    // stdlib type claims this node (`@[Literal( FuncType )]`), and a callable
    // has exactly one result but any number of parameters — only a leading
    // result sits at a fixed index a signature can name.
    struct FuncType
    {

        Core::SourceRange Loc;
        TypeId Return;
        TypeList Params;
    };

    enum class TypeKind
    {

        None,
#define VOLT_TYPE( Name ) Name,
#include "Volt/Frontend/AST/Nodes.inl"
    };

    using TypeNode = std::variant<std::monostate
#define VOLT_TYPE( Name ) , Name
#include "Volt/Frontend/AST/Nodes.inl"
                                  >;

    [[nodiscard]] constexpr TypeKind KindOf ( const TypeNode &Node )
    {
        return static_cast<TypeKind>( Node.index() );
    }

} // namespace Frontend

} // namespace Volt
