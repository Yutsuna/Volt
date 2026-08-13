// MagicExpansion.cpp — Order 16 Lowering pass: inlines the compiler-injected
// constants (`__FILE__`, `__LINE__`, …) into ordinary literal nodes.
//
// The whole vocabulary lives in MagicConstants.inl; this file contains no
// per-constant branch. Adding `__DIR__` was one manifest line, and so is the
// next one. The expansion targets existing literal nodes on purpose: typing
// then falls out of the zero-hardcode @[Literal( ... )] binding in the Volt
// stdlib, so no Volt type name is ever mentioned here.
//
// Why order 16, right after MacroExpansion (15):
//   - macro-generated text gets its constants inlined too;
//   - the re-parsed nodes carry the *invocation* SourceRange, so `__LINE__`
//     inside a macro body reports the caller's line, the C-like semantics;
//   - ScopeResolver (10) already ran, but it is deliberately tolerant of
//     names it cannot bind (a counter, never a diagnostic), so nothing
//     complains about `__FILE__` in between.
//
// `__FUNCTION__` is why this is a proper declaration walk and not a flat
// sweep of the expression arena: the arena does not say which method encloses
// which expression. Only Method is special-cased; everything else falls
// through the Reflect-driven field walk, so a new AST node is free here.

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Core/Support/Version.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/MiddleEnd/ConstEval/MagicConstants.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace
{

using namespace Volt;
using namespace Volt::MiddleEnd::ConstEval;
using PassContext = Volt::MiddleEnd::Core::PassContext;

// The only two shapes a manifest Value column may produce. Everything
// a literal node stores is an interned string, so a numeric site
// field renders through the second overload and nothing else is
// needed to widen MagicSite.
[[nodiscard]] std::string MagicText ( std::string_view Text )
{
    return std::string{ Text };
}

[[nodiscard]] std::string MagicText ( std::uint32_t Value )
{
    return std::to_string( Value );
}

[[nodiscard]] bool IsMagicInterior ( const char Character )
{
    return ( Character >= 'A' and Character <= 'Z' ) or ( Character >= '0' and Character <= '9' ) or Character == '_';
}

} // namespace

std::optional<Frontend::ExprNode> Volt::MiddleEnd::ConstEval::ExpandMagic ( std::string_view Name,
                                                                            const MagicSite &Site,
                                                                            ::Volt::Core::StringInterner &Interner,
                                                                            ::Volt::Core::SourceRange Loc )
{
    // Every literal node reachable from the manifest has the same shape —
    // { SourceRange, Symbol } — which is what lets one construction
    // expression serve them all, generated straight from the manifest.
#define VOLT_MAGIC( Spelling, Node, Value )                                                                                      \
    if ( Name == std::string_view( Spelling ) )                                                                                  \
    {                                                                                                                            \
        const Frontend::Node Lit{ Loc, Interner.Intern( MagicText( Value ) ) };                                                  \
        return Frontend::ExprNode{ Lit };                                                                                        \
    }
#include "Volt/MiddleEnd/ConstEval/MagicConstants.inl"

    return std::nullopt;
}

bool Volt::MiddleEnd::ConstEval::IsMagicShape ( std::string_view Name )
{
    if ( Name.size() <= 4 or not Name.starts_with( "__" ) or not Name.ends_with( "__" ) )
    {
        return false;
    }

    return std::ranges::all_of( Name.substr( 2, Name.size() - 4 ), IsMagicInterior );
}

std::span<const std::string_view> Volt::MiddleEnd::ConstEval::MagicNames ()
{
#define VOLT_MAGIC( Spelling, Node, Value ) std::string_view( Spelling ),
    static constexpr std::array Names{
#include "Volt/MiddleEnd/ConstEval/MagicConstants.inl"
    };
    return Names;
}

namespace
{

using namespace Volt;
using namespace Volt::MiddleEnd::ConstEval;
using PassContext = Volt::MiddleEnd::Core::PassContext;

class Expander
{

public:

    explicit Expander ( PassContext &InContext ) : Context( InContext )
    {
    }

    void Run ()
    {
        // Answering "where am I" needs the file table; a tool that
        // never registered one gets a silent no-op rather than a
        // wrong answer.
        if ( Context.Sources == nullptr or not Context.Sources->IsValidFile( Context.Ast.FileId() ) )
        {
            return;
        }

        Path = Context.Sources->PathOf( Context.Ast.FileId() );

        const std::size_t Slash = Path.find_last_of( '/' );
        Dir                     = Slash == std::string_view::npos ? std::string_view{} : Path.substr( 0, Slash );

        for ( const Frontend::DeclId Id : Context.Ast.TopDecls )
        {
            WalkDecl( Id, Frontend::Symbol{} );
        }
        for ( const Frontend::StmtId Id : Context.Ast.TopStmts )
        {
            WalkStmt( Id, Frontend::Symbol{} );
        }
    }

private:

    // Function is the enclosing method's name, threaded down the walk
    // — the one piece of context an expansion cannot read off a node.
    void WalkDecl ( Frontend::DeclId Id, Frontend::Symbol Function )
    {
        if ( not Id.IsValid() )
        {
            return;
        }

        std::visit(
            Meta::Overloaded{
                [&] ( const Frontend::Method &Node )
                {
                    for ( const Frontend::ParamId Param : Node.Params )
                    {
                        WalkExpr( Context.Ast.GetParam( Param ).Default, Node.Name );
                    }
                    for ( const Frontend::StmtId Child : Node.Body )
                    {
                        WalkStmt( Child, Node.Name );
                    }
                },
                [&] ( const auto &Node ) { WalkFields( Node, Function ); },
            },
            Context.Ast.Decl( Id ) );
    }

    void WalkStmt ( Frontend::StmtId Id, Frontend::Symbol Function )
    {
        if ( not Id.IsValid() )
        {
            return;
        }
        std::visit( [&] ( const auto &Node ) { WalkFields( Node, Function ); }, Context.Ast.Stmt( Id ) );
    }

    void WalkExpr ( Frontend::ExprId Id, Frontend::Symbol Function )
    {
        if ( not Id.IsValid() )
        {
            return;
        }

        // Children first: an expansion replaces the node wholesale,
        // so descending afterwards would walk the freshly-built
        // literal instead of the original operands.
        std::visit( [&] ( const auto &Node ) { WalkFields( Node, Function ); }, Context.Ast.Expr( Id ) );

        const auto *Name = std::get_if<Frontend::Identifier>( &Context.Ast.Expr( Id ) );
        if ( Name == nullptr )
        {
            return;
        }

        const std::string_view Spelling = Context.Ast.Text( Name->Name );
        if ( not IsMagicShape( Spelling ) )
        {
            return;
        }

        const ::Volt::Core::SourceRange Loc = Name->Loc;
        const Volt::Core::LineColumn Where  = Context.Sources->Resolve( Loc.File, Loc.Begin );

        const MagicSite Site{ .Path     = Path,
                              .Dir      = Dir,
                              .Function = Function.IsValid() ? Context.Ast.Text( Function ) : std::string_view{},
                              .Line     = Where.Line,
                              .Column   = Where.Column };

        std::optional<Frontend::ExprNode> Expanded = ExpandMagic( Spelling, Site, Context.Ast.Strings(), Loc );
        if ( not Expanded.has_value() )
        {
            ReportUnknown( Spelling, Loc );
            return;
        }

        // Rewrite in place: parents hold the ExprId, so the whole tree
        // updates at once and traversal order is irrelevant.
        Context.Ast.Expr( Id ) = *std::move( Expanded );
        ++Context.Stats.MagicsExpanded;
    }

    // The reserved `__NAME__` shape belongs to the compiler, so a
    // spelling of that shape the manifest does not know is a typo,
    // reported here rather than left to surface as a puzzling
    // unknown-identifier error much later.
    void ReportUnknown ( std::string_view Spelling, ::Volt::Core::SourceRange Loc )
    {
        std::string Known;
        for ( const std::string_view Candidate : MagicNames() )
        {
            if ( not Known.empty() )
            {
                Known += ", ";
            }
            Known += Candidate;
        }

        Context.Diags.Report( Volt::Core::Diagnostic{
            .Severity = Volt::Core::ESeverity::Error,
            .Range    = Loc,
            .Message  = "unknown magic constant '" + std::string{ Spelling } + "'",
            .Notes    = { Volt::Core::DiagnosticNote{ .Range = Loc, .Message = "known magic constants: " + Known } } } );
    }

    // Reflection-driven default walk: recurse into every child node a
    // field carries, whatever the node is called.
    template <typename NodeType> void WalkFields ( const NodeType &Node, Frontend::Symbol Function )
    {
        if constexpr ( Meta::Reflected<NodeType> )
        {
            Meta::ForEachField( Node,
                                [&] ( std::string_view, const auto &Field )
                                {
                                    using FieldType = std::remove_cvref_t<decltype( Field )>;
                                    if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                    {
                                        WalkExpr( Field, Function );
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                    {
                                        for ( const Frontend::ExprId Child : Field )
                                        {
                                            WalkExpr( Child, Function );
                                        }
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Frontend::StmtId> )
                                    {
                                        WalkStmt( Field, Function );
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Frontend::StmtList> )
                                    {
                                        for ( const Frontend::StmtId Child : Field )
                                        {
                                            WalkStmt( Child, Function );
                                        }
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Frontend::DeclList> )
                                    {
                                        for ( const Frontend::DeclId Child : Field )
                                        {
                                            WalkDecl( Child, Function );
                                        }
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Frontend::ParamList> )
                                    {
                                        for ( const Frontend::ParamId Child : Field )
                                        {
                                            WalkExpr( Context.Ast.GetParam( Child ).Default, Function );
                                        }
                                    }
                                } );
        }
    }

    PassContext &Context;
    std::string_view Path;
    std::string_view Dir;
};

} // namespace

namespace Volt::MiddleEnd::ConstEval
{

void MagicExpansion ( PassContext &Context )
{
    Expander Walk{ Context };
    Walk.Run();
}

} // namespace Volt::MiddleEnd::ConstEval
