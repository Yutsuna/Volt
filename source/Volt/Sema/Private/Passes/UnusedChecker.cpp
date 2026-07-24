// UnusedChecker.cpp — diagnoses unreferenced local variables and parameters.
//
// Order 35, after TypeChecker (30): all inference has settled, and
// ScopeResolver (10) has published UseCounts. A binding whose count is
// zero and whose name does not begin with '_' gets a warning. The pass
// is a pure read of the ScopeTable — no AST mutation, no type access.

#include "Volt/Sema/Pass.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"

/**
 * Private
 */

namespace
{

Volt::Core::SourceRange LocOfSite ( const Volt::Frontend::AstContext &Ast, const Volt::Sema::BindingSite &Site )
{
    using namespace Volt;

    return std::visit(
        Meta::Overloaded{
            [&] ( Frontend::StmtId Id ) -> Core::SourceRange
            { return Id.IsValid() ? Frontend::LocOf( Ast.Stmt( Id ) ) : Core::SourceRange{}; },
            [&] ( Frontend::ParamId Id ) -> Core::SourceRange
            { return Id.IsValid() ? Ast.GetParam( Id ).Loc : Core::SourceRange{}; },
            [&] ( Frontend::DeclId Id ) -> Core::SourceRange
            { return Id.IsValid() ? Frontend::LocOf( Ast.Decl( Id ) ) : Core::SourceRange{}; },
        },
        Site );
}

} // namespace

/**
 * Public
 */

void Volt::Sema::UnusedChecker ( PassContext &Context )
{
    for ( std::size_t I = 0; I < Context.Scopes.Size(); ++I )
    {
        const ScopeId Id{ static_cast<std::uint32_t>( I ) };
        const Scope &S = Context.Scopes.Get( Id );

        // Only check Method and Block scopes — Unit and Type bindings
        // are public API / field declarations, never "unused" in the
        // local sense. Branch scopes inherit their parent's bindings
        // and do not declare locals of their own.
        if ( S.Kind != EScopeKind::Method and S.Kind != EScopeKind::Block )
        {
            continue;
        }

        for ( const Symbol Name : S.Order )
        {
            const auto It = S.Bindings.find( Name );
            if ( It == S.Bindings.end() )
            {
                continue;
            }

            const Binding &Entry = It->second;

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

            Context.Diags.Report( Core::Diagnostic{
                .Severity = Core::ESeverity::Warning,
                .Range    = LocOfSite( Context.Ast, Entry.Site ),
                .Message  = "unused " + Kind + " '" + std::string{ Spelling } + "' — prefix with '_' if intentional",
                .Notes    = {},
            } );
        }
    }
}
