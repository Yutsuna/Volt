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

        Core::SourceRange Loc;
        Symbol Name;
        DeclList Body;
    };

    // `class Name[Generics] < Super ... end`. Super is invalid when absent.
    struct Class
    {

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        TypeId Super;
        DeclList Body;
    };

    struct Struct
    {

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        DeclList Body;
    };

    struct Mixin
    {

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        DeclList Body;
    };

    // `[abstract] def [self.]Name(Params) -> ReturnType ... end`.
    struct Method
    {

        Core::SourceRange Loc;
        Symbol Name;
        ParamList Params;
        TypeId ReturnType;
        StmtList Body;
        bool bSelf     = false; // `def self.name`
        bool bAbstract = false;
    };

    // Instance-variable / accessor declaration: `x : T`, `getter x : T`,
    // `property y : T [= default]`.
    struct Field
    {

        Core::SourceRange Loc;
        Symbol Name;
        TypeId DeclType;
        ExprId Default;
        EAccessor Accessor = EAccessor::None;
    };

    // `include Target`
    struct Include
    {

        Core::SourceRange Loc;
        TypeId Target;
    };

    // `component Name(Params) <jsx> end`
    struct Component
    {

        Core::SourceRange Loc;
        Symbol Name;
        ParamList Params;
        StmtList Body;
    };

    // `circuit "Name" { ... }` — the project/circuit manifest block.
    struct Circuit
    {

        Core::SourceRange Loc;
        Symbol Name;
        StmtList Body;
    };

    // `@[Name(Args...)]`
    struct Annotation
    {

        Core::SourceRange Loc;
        Symbol Name;
        ExprList Args;
    };

    // `macro def Name(Params) ... end`. The body is not parsed: it is the raw
    // source slice between the header and the matching `end`, interned as-is.
    // The MacroExpansion pass evaluates its {% %} / {{ }} template tags at
    // compile time and re-parses the generated text.
    struct MacroDef
    {

        Core::SourceRange Loc;
        Symbol Name;
        ParamList Params;
        Symbol BodyText;
    };

    // `name( args... )` in declaration position — a compile-time macro
    // invocation whose expansion replaces this slot in the enclosing body.
    struct MacroInvoke
    {

        Core::SourceRange Loc;
        Symbol Name;
        ExprList Args;
        SymbolList ArgNames;
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

} // namespace Frontend

} // namespace Volt
