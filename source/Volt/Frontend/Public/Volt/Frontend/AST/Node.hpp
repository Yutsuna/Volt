#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Core/Support/Id.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Core/Support/StringInterner.hpp"

#include <string_view>
#include <variant>

namespace Volt
{

namespace Frontend
{

    // One strongly-typed Id family per AST category. References between
    // nodes are these u32 handles into the AstContext arenas — never
    // pointers, so an AstContext is flat, copyable and serialisable.
    struct ExprTag
    {
    };
    struct StmtTag
    {
    };
    struct DeclTag
    {
    };
    struct TypeTag
    {
    };
    struct ParamTag
    {
    };

    using ExprId  = Core::TypedId<ExprTag>;
    using StmtId  = Core::TypedId<StmtTag>;
    using DeclId  = Core::TypedId<DeclTag>;
    using TypeId  = Core::TypedId<TypeTag>;
    using ParamId = Core::TypedId<ParamTag>;

    using Core::Symbol;

    // Small inline capacity: most child lists hold only a handful of nodes.
    template <typename T> using NodeList = Core::SmallVec<T, 4>;

    using ExprList   = NodeList<ExprId>;
    using StmtList   = NodeList<StmtId>;
    using DeclList   = NodeList<DeclId>;
    using TypeList   = NodeList<TypeId>;
    using ParamList  = NodeList<ParamId>;
    using SymbolList = NodeList<Symbol>;

    /// Accessor kind for a Field declaration (`getter`/`setter`/`property`/plain ivar).
    enum class EAccessor
    {

        None,
        Getter,
        Setter,
        Property,
    };

    /// A method / component parameter. Stored in its own arena (ParamId)
    /// rather than the node variants, since it is shared shape, not a node.
    struct Param
    {

        Core::SourceRange Loc;
        Symbol Name;
        TypeId DeclType;
        ExprId Default;
        bool bInstanceVar = false; // `def initialize(@x : T)`
        bool bIsBlock     = false; // `def each(&block : T -> Void)`
    };

    /// Name of a node variant's active alternative. One reflective template
    /// replaces the former per-category X-macro switches.
    template <typename... Alts> [[nodiscard]] constexpr std::string_view NodeName ( const std::variant<Alts...> &Node )
    {
        return Meta::ActiveName( Node );
    }

} // namespace Frontend

} // namespace Volt
