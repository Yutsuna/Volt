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

    // `typeof( expr )` — the type `expr` has, written where a type is written.
    //
    // The one type node whose operand is a *value*: every other shape here is
    // built from types alone, which is what lets `ResolveTypeExpr` resolve them
    // all through one reflected line. This one cannot go that way — answering
    // it means inferring an expression, and inference lives above the type
    // system — so `UnitSink` carries a hook back into `Analysis` and this node
    // is the only branch that consults it. A sink with no hook (`SigSink`, a
    // declaration's own signature) cannot answer it and says so.
    //
    // `Value` is never walked by the TypeChecker: it sits inside an annotation,
    // not in expression position, so nothing else infers it and the hook is
    // what gives it a type at all.
    struct TypeOfType
    {

        Core::SourceRange Loc;
        ExprId Value;
    };

    // `Dynamic<T>` / `&Trait` — dynamic polymorphic fat pointer.
    struct DynamicType
    {

        Core::SourceRange Loc;
        TypeId Trait;
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
