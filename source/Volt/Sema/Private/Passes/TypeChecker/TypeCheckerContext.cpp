#include "TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"

/**
 * Ctor
 */

Volt::Sema::TypeCheckerPass::TypeCheckerContext::TypeCheckerContext ( PassContext &InCtx, std::vector<bool> InMetadata )
    : Ctx( InCtx ), Metadata( std::move( InMetadata ) )
{
}

/**
 * Public
 */

void Volt::Sema::TypeCheckerPass::TypeCheckerContext::Report ( Core::SourceRange Loc, std::string Message )
{
    Ctx.Diags.Report(
        Core::Diagnostic{ .Severity = Core::ESeverity::Error, .Range = Loc, .Message = std::move( Message ), .Notes = {} } );
}

std::optional<Volt::Sema::SemaTypeId> Volt::Sema::TypeCheckerPass::TypeCheckerContext::FindLocal ( Frontend::ExprId Use,
                                                                                                   Symbol Name ) const
{
    SemaTypeId FoundType;

    // A use ScopeResolver bound answers from its site and from nowhere else.
    // Falling back to the name would make two locals that merely share a
    // spelling one variable: `b = *( p + i )` in two sibling `while` bodies is
    // two declarations, two sites, and the second must be typed on its own —
    // the name map would report the first one's type as already known and the
    // second site would never be typed at all. The name map answers only for
    // an identifier the ScopeTable cannot know (a node minted after Order 10).
    const std::optional<BindingSite> Site = SiteOf( Use, Name );
    if ( Site.has_value() )
    {
        if ( const auto It = LocalTypes.find( *Site ); It != LocalTypes.end() )
        {
            FoundType = It->second;
        }
    }
    else if ( const auto It = Locals.find( Name ); It != Locals.end() )
    {
        FoundType = It->second;
    }
    if ( not FoundType.IsValid() )
    {
        return std::nullopt;
    }

    // Definite assignment: warn when a variable declared without an
    // initializer is read before its first assignment.
    if ( UninitializedLocals.contains( Name ) )
    {
        Ctx.Diags.Report( Core::Diagnostic{
            .Severity = Core::ESeverity::Warning,
            .Range    = Use.IsValid() ? Frontend::LocOf( Ctx.Ast.Expr( Use ) ) : Core::SourceRange{},
            .Message  = "variable '" + std::string{ Ctx.Ast.Text( Name ) } + "' used before being initialized",
            .Notes    = {},
        } );
    }

    return FoundType;
}

std::optional<Volt::Sema::BindingSite> Volt::Sema::TypeCheckerPass::TypeCheckerContext::SiteOf ( Frontend::ExprId Use,
                                                                                                 Symbol Name ) const
{
    if ( const Binding *Bound = Ctx.Scopes.BindingOf( Use ) )
    {
        return Bound->Site;
    }
    if ( const auto It = LocalSites.find( Name ); It != LocalSites.end() )
    {
        return It->second;
    }
    return std::nullopt;
}

void Volt::Sema::TypeCheckerPass::TypeCheckerContext::WriteLocal ( Frontend::ExprId Use, Symbol Name, SemaTypeId Type )
{
    // A use ScopeResolver bound is authoritative, and it also *teaches* the
    // name index, so a later write through a node minted after Order 10 —
    // which can carry no binding — still lands on the same site rather than
    // forking a second, name-only answer.
    if ( const Binding *Bound = Ctx.Scopes.BindingOf( Use ) )
    {
        LocalSites[Name] = Bound->Site;
    }
    if ( const std::optional<BindingSite> Site = SiteOf( Use, Name ) )
    {
        LocalTypes[*Site] = Type;
        Ctx.Values.SetSiteType( *Site, Type );
    }
    Locals[Name] = Type;
}

std::span<const Volt::Sema::Symbol> Volt::Sema::TypeCheckerPass::TypeCheckerContext::Generics () const
{
    if ( SelfGenerics == nullptr )
    {
        return {};
    }
    return std::span<const Symbol>{ SelfGenerics->begin(), SelfGenerics->Size() };
}

std::string Volt::Sema::TypeCheckerPass::TypeCheckerContext::NameOf ( NominalId Id ) const
{
    if ( not Id.IsValid() )
    {
        return "<unresolved>";
    }
    return std::string{ Ctx.Types.Text( Ctx.Types.Type( Id ).Name ) };
}

std::string Volt::Sema::TypeCheckerPass::TypeCheckerContext::NameOfValue ( SemaTypeId Id ) const
{
    if ( not Ctx.Values.Has( Id ) )
    {
        return "<unresolved>";
    }
    return NameOf( Ctx.Values.Get( Id ).Base );
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::TypeCheckerContext::MakeType ( NominalId Base,
                                                                                   Core::SmallVec<SemaTypeId, 2> Args )
{
    return Ctx.Values.Intern( SemaType{ .Base = Base, .Args = std::move( Args ) } );
}
