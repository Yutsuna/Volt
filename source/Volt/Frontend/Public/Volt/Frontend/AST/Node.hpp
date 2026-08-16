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

    // `<T, U : Bound>`: one generic parameter list, parsed once and reused by
    // every declaration shape that opens one (class/struct/mixin/enum/method).
    // `Bounds` is parallel to `Names` — an invalid TypeId at index `i` means
    // parameter `i` has no bound. Kept as a plain aggregate (not a NodeList
    // element) since it is a parser-side return value, never stored in an
    // arena of its own.
    struct GenericParamList
    {

        SymbolList Names;
        TypeList Bounds;
    };

    /// Accessor kind for a Field declaration (`getter`/`setter`/`property`/plain ivar).
    enum class EAccessor
    {

        None,
        Getter,
        Setter,
        Property,
    };

    /// Who may reach a Field or a Method: `private` (the declaring type only),
    /// `protected` (that type and everything below it), `public` (anyone).
    ///
    /// `None` is *nothing written*, exactly as `EAccessor::None` is a plain
    /// ivar — the parser records the syntax and the reader applies the
    /// default. Both readers already agree on what that default is:
    /// `Analysis::CheckMemberAccess` waves None through as public, and
    /// AstDump prints nothing for a `None`-valued enum, which is why adding
    /// this field left every existing golden byte-identical.
    enum class EVisibility
    {

        None,
        Public,
        Protected,
        Private,
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
