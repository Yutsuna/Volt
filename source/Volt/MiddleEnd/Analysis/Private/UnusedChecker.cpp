// UnusedChecker.cpp — diagnoses unreferenced local variables and parameters.
//
// Order 35, after TypeChecker (30): all inference has settled, and
// ScopeResolver (10) has published UseCounts. A binding whose count is
// zero and whose name does not begin with '_' gets a warning. The pass
// is a pure read of the ScopeTable — no AST mutation, no type access.

#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"

/**
 * Private
 */

namespace
{

Volt::Core::SourceRange LocOfSite ( const Volt::Frontend::AstContext &Ast, const Volt::MiddleEnd::Resolver::BindingSite &Site )
{
    using namespace Volt;

    return std::visit(
        Meta::Overloaded{
            [&] ( Frontend::StmtId Id ) -> Volt::Core::SourceRange
            { return Id.IsValid() ? Frontend::LocOf( Ast.Stmt( Id ) ) : Volt::Core::SourceRange{}; },
            [&] ( Frontend::ParamId Id ) -> Volt::Core::SourceRange
            { return Id.IsValid() ? Ast.GetParam( Id ).Loc : Volt::Core::SourceRange{}; },
            [&] ( Frontend::DeclId Id ) -> Volt::Core::SourceRange
            { return Id.IsValid() ? Frontend::LocOf( Ast.Decl( Id ) ) : Volt::Core::SourceRange{}; },
            // An implicit local: the site is the Assign target that declared it.
            [&] ( Frontend::ExprId Id ) -> Volt::Core::SourceRange
            { return Id.IsValid() ? Frontend::LocOf( Ast.Expr( Id ) ) : Volt::Core::SourceRange{}; },
        },
        Site );
}

} // namespace

/**
 * Public
 */

void Volt::MiddleEnd::Analysis::UnusedChecker ( Core::PassContext &Context )
{
    // ScopeId/Scope/EScopeKind/Symbol resolve unqualified below; Resolver
    // is a sibling module of Analysis, not an ancestor, so ordinary lookup
    // needs these directives.
    using namespace Volt::MiddleEnd::Resolver;
    using namespace Volt::Core;

    for ( std::size_t Idx = 0; Idx < Context.Scopes.Size(); ++Idx )
    {
        const ScopeId Id{ static_cast<std::uint32_t>( Idx ) };
        const Scope &Scope = Context.Scopes.Get( Id );

        // Only check Method and Block scopes — Unit and Type bindings
        // are public API / field declarations, never "unused" in the
        // local sense. Branch scopes inherit their parent's bindings
        // and do not declare locals of their own.
        if ( Scope.Kind != EScopeKind::Method and Scope.Kind != EScopeKind::Block )
        {
            continue;
        }

        for ( const Symbol Name : Scope.Order )
        {
            const auto It = Scope.Bindings.find( Name );
            if ( It == Scope.Bindings.end() )
            {
                continue;
            }

            const Binding &Entry = It->second;

            if ( const auto *ParamIdPtr = std::get_if<Frontend::ParamId>( &Entry.Site ) )
            {
                if ( Context.Ast.GetParam( *ParamIdPtr ).bInstanceVar )
                {
                    continue;
                }
            }

            // Convention: names starting with '_' are intentionally unused.
            const std::string_view Spelling = Context.Ast.Text( Name );
            if ( Spelling.empty() or Spelling.front() == '_' )
            {
                continue;
            }

            if ( Context.Scopes.UseCountOf( Entry.Site ) != 0 )
            {
                continue;
            }

            const bool bIsParam    = std::holds_alternative<Frontend::ParamId>( Entry.Site );
            const std::string Kind = bIsParam ? "parameter" : "variable";

            // Fully qualified from the root: inside Volt::MiddleEnd::Analysis
            // (this function's own, already-migrated namespace), unqualified
            // `Core::` would resolve to the sibling Volt::MiddleEnd::Core
            // instead of Volt::Core, which owns Diagnostic/ESeverity.
            Context.Diags.Report( ::Volt::Core::Diagnostic{
                .Severity = ::Volt::Core::ESeverity::Warning,
                .Range    = LocOfSite( Context.Ast, Entry.Site ),
                .Message  = "unused " + Kind + " '" + std::string{ Spelling } + "' — prefix with '_' if intentional",
                .Notes    = {},
            } );
        }
    }
}
