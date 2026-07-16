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
        // `Core::AppConfig`, `Array[T]`, `Pointer[T]`, `Void`.
        struct TypeRef
        {

            using Self = TypeRef;

            Core::SourceRange Loc;
            SymbolList        Path;
            TypeList          Generics;

            VOLT_FIELDS( Path, Generics )
        };

        // `T*` — pointer to Pointee.
        struct PointerType
        {

            using Self = PointerType;

            Core::SourceRange Loc;
            TypeId            Pointee;

            VOLT_FIELDS( Pointee )
        };

        // `T?` — nilable Inner.
        struct NilableType
        {

            using Self = NilableType;

            Core::SourceRange Loc;
            TypeId            Inner;

            VOLT_FIELDS( Inner )
        };

        // `T[N]` — fixed-size array (Size is a constant expression).
        struct FixedArrayType
        {

            using Self = FixedArrayType;

            Core::SourceRange Loc;
            TypeId            Elem;
            ExprId            Size;

            VOLT_FIELDS( Elem, Size )
        };

        // `(A, B) -> R` / `-> R` — function type.
        struct FuncType
        {

            using Self = FuncType;

            Core::SourceRange Loc;
            TypeList          Params;
            TypeId            Return;

            VOLT_FIELDS( Params, Return )
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

        [[nodiscard]] constexpr TypeKind KindOf( const TypeNode& Node )
        {
            return static_cast<TypeKind>( Node.index() );
        }

        [[nodiscard]] constexpr std::string_view NodeName( const TypeNode& Node )
        {
            switch ( KindOf( Node ) )
            {
                case TypeKind::None:
                    return "None";
#define VOLT_TYPE( Name )                                                                                                          \
    case TypeKind::Name:                                                                                                          \
        return #Name;
#include "Volt/Frontend/AST/Nodes.inl"
            }
            return "?";
        }

    }

}
