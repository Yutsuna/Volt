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
        // Parallel to Generics: `Generics[i] : GenericBounds[i]`, invalid
        // when parameter `i` has no bound (`T`, not `T : Finalizable`).
        TypeList GenericBounds;
        TypeId Super;
        DeclList Body;
    };

    struct Struct
    {

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        TypeList GenericBounds;
        DeclList Body;
    };

    struct Mixin
    {

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        TypeList GenericBounds;
        DeclList Body;
    };

    // `[abstract] def [self.]Name(Params) -> ReturnType ... end`.
    struct Method
    {

        Core::SourceRange Loc;
        Symbol Name;
        // `def map<U>( ... )`: parameters of the method itself, on top of
        // whatever the enclosing type declares. They are what lets a
        // signature name a type the receiver does not fix — `U` is decided
        // by the call, not by `Array<T>`.
        SymbolList Generics;
        ParamList Params;
        TypeId ReturnType;
        StmtList Body;
        bool bSelf = false; // `def self.name`
        // A contract a including type must honour (`mixin` member). Nobody
        // implements it here, and the type checker requires an override.
        bool bAbstract = false;
        // Declared here, implemented outside Volt — the `@[External(...)]`
        // annotation names the symbol to link against. Body-less like an
        // abstract method, but the opposite resolution: never overridden.
        bool bExternal = false;
        // `private def` / a `private` section above it. None when the source
        // wrote neither, which reads as public.
        EVisibility Visibility = EVisibility::None;
    };

    // Instance-variable / accessor declaration: `x : T`, `getter x : T`,
    // `property y : T [= default]`.
    struct Field
    {

        Core::SourceRange Loc;
        Symbol Name;
        TypeId DeclType;
        ExprId Default;
        // Ahead of Accessor so a dump reads in source order — `private
        // getter`, never `getter private`.
        EVisibility Visibility = EVisibility::None;
        EAccessor Accessor     = EAccessor::None;
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

    // `macro def Name( Params ) -> Type ... end` — a method whose body is a
    // compile-time program. Written in a type or mixin body, it is evaluated
    // once per concrete target type: what the evaluation emits becomes that
    // type's real `Method`, registered as one of its members
    // (ConstEval::ExpandTypeMacros, at the interface seam). Params and
    // ReturnType are the *generated method's* own signature — they are runtime,
    // not compile-time arguments.
    //
    // The body is ordinary parsed Volt, not a text template: control flow over
    // compile-time values steers the generation, and everything else is emitted
    // (rules of the macro model, .agents/middleend/macros.md).
    struct MacroDef
    {

        Core::SourceRange Loc;
        Symbol Name;
        ParamList Params;
        TypeId ReturnType;
        StmtList Body;
        bool bSelf             = false; // `macro def self.name`
        EVisibility Visibility = EVisibility::None;
    };

    // `macro do ... end` at file scope — a compile-time program run for its
    // effects alone (host commands, compiler-side output, build validation).
    // It emits nothing: no member, no statement, no byte in the binary.
    struct MacroBlock
    {

        Core::SourceRange Loc;
        StmtList Body;
    };

    // `enum Name[Generics] [: Underlying] ... end`. Underlying is invalid when
    // omitted — resolved later from stdlib annotations, never guessed in C++.
    struct Enum
    {

        Core::SourceRange Loc;
        Symbol Name;
        SymbolList Generics;
        TypeId Underlying;
        DeclList Body;
    };

    // `Name[( params )] [= expr]` — a single enum member, optionally carrying a
    // payload (`Some( value : T )`) and/or an explicit value (`Ok = 200`).
    struct EnumCase
    {

        Core::SourceRange Loc;
        Symbol Name;
        ParamList Payload;
        ExprId Value;
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
