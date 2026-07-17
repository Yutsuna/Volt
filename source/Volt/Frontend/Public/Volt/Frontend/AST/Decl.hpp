#pragma once

#include "Volt/Frontend/AST/Node.hpp"

#include <cstddef>
#include <string_view>
#include <variant>

namespace Volt
{

namespace Frontend
{

    struct Module
    {

        using Self = Module;

        Core::SourceRange Loc;
        Symbol Name;
        DeclList Body;

        VOLT_FIELDS( Name, Body )
    };

    // `class Name[Generics] < Super ... end`. Super is invalid when absent.
    struct Class
    {

        using Self = Class;

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        TypeId Super;
        DeclList Body;

        VOLT_FIELDS( Name, Generics, Super, Body )
    };

    struct Struct
    {

        using Self = Struct;

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        DeclList Body;

        VOLT_FIELDS( Name, Generics, Body )
    };

    struct Mixin
    {

        using Self = Mixin;

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        DeclList Body;

        VOLT_FIELDS( Name, Generics, Body )
    };

    // `[abstract] def [self.]Name(Params) -> ReturnType ... end`.
    struct Method
    {

        using Self = Method;

        Core::SourceRange Loc;
        Symbol Name;
        ParamList Params;
        TypeId ReturnType;
        StmtList Body;
        bool bSelf     = false; // `def self.name`
        bool bAbstract = false;

        VOLT_FIELDS( Name, Params, ReturnType, Body, bSelf, bAbstract )
    };

    // Instance-variable / accessor declaration: `x : T`, `getter x : T`,
    // `property y : T [= default]`.
    struct Field
    {

        using Self = Field;

        Core::SourceRange Loc;
        Symbol Name;
        TypeId DeclType;
        ExprId Default;
        EAccessor Accessor = EAccessor::None;

        VOLT_FIELDS( Name, DeclType, Default, Accessor )
    };

    // `include Target`
    struct Include
    {

        using Self = Include;

        Core::SourceRange Loc;
        TypeId Target;

        VOLT_FIELDS( Target )
    };

    // `component Name(Params) <jsx> end`
    struct Component
    {

        using Self = Component;

        Core::SourceRange Loc;
        Symbol Name;
        ParamList Params;
        StmtList Body;

        VOLT_FIELDS( Name, Params, Body )
    };

    // `circuit "Name" { ... }` — the project/circuit manifest block.
    struct Circuit
    {

        using Self = Circuit;

        Core::SourceRange Loc;
        Symbol Name;
        StmtList Body;

        VOLT_FIELDS( Name, Body )
    };

    // `@[Name(Args...)]`
    struct Annotation
    {

        using Self = Annotation;

        Core::SourceRange Loc;
        Symbol Name;
        ExprList Args;

        VOLT_FIELDS( Name, Args )
    };

    enum class DeclKind
    {

        None,
#define VOLT_DECL( Name ) Name,
#include "Volt/Frontend/AST/Nodes.inl"
    };

    using DeclNode = std::variant<std::monostate
#define VOLT_DECL( Name ) , Name
#include "Volt/Frontend/AST/Nodes.inl"
                                  >;

    [[nodiscard]] constexpr DeclKind KindOf ( const DeclNode &Node )
    {
        return static_cast<DeclKind>( Node.index() );
    }

    [[nodiscard]] constexpr std::string_view NodeName ( const DeclNode &Node )
    {
        switch ( KindOf( Node ) )
        {
        case DeclKind::None:
            return "None";
#define VOLT_DECL( Name )                                                                                                                                                \
    case DeclKind::Name:                                                                                                                                                 \
        return #Name;
#include "Volt/Frontend/AST/Nodes.inl"
        }
        return "?";
    }

} // namespace Frontend

} // namespace Volt
