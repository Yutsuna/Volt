#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"

/**
 * Ctor
 */

Volt::MiddleEnd::Analysis::TypeCheckerContext::TypeCheckerContext ( PassContext &InCtx, std::vector<bool> InMetadata )
    : Ctx( InCtx ), Metadata( std::move( InMetadata ) )
{
}

/**
 * Public
 */

void Volt::MiddleEnd::Analysis::TypeCheckerContext::Report ( Volt::Core::SourceRange Loc, std::string Message )
{
    Ctx.Diags.Report( Volt::Core::Diagnostic{
        .Severity = Volt::Core::ESeverity::Error, .Range = Loc, .Message = std::move( Message ), .Notes = {} } );
}

std::optional<Volt::MiddleEnd::TypeSystem::SemaTypeId>
Volt::MiddleEnd::Analysis::TypeCheckerContext::FindLocal ( Frontend::ExprId Use, Symbol Name ) const
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
        else if ( const Binding *Bound = Ctx.Scopes.BindingOf( Use );
                  Bound != nullptr and Ctx.Scopes.Get( Bound->Owner ).Kind == EScopeKind::Unit )
        {
            // A file is a module and its top-level locals are its globals
            // (rules/core-ast.md's sibling decision): `EnterMethod` clears
            // `LocalTypes` for every `def`, but a Unit-scope site was already
            // typed once while the file's top-level statements ran, and that
            // typing lives in `Ctx.Values` (never reset) — exactly what the
            // backend itself reads for a Unit-scope binding (`SlotFor`,
            // StmtEmitter.cpp).
            FoundType = Ctx.Values.SiteType( *Site );
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
        Ctx.Diags.Report( Volt::Core::Diagnostic{
            .Severity = Volt::Core::ESeverity::Warning,
            .Range    = Use.IsValid() ? Frontend::LocOf( Ctx.Ast.Expr( Use ) ) : Volt::Core::SourceRange{},
            .Message  = "variable '" + std::string{ Ctx.Ast.Text( Name ) } + "' used before being initialized",
            .Notes    = {},
        } );
    }

    return FoundType;
}

std::optional<Volt::MiddleEnd::Resolver::BindingSite>
Volt::MiddleEnd::Analysis::TypeCheckerContext::SiteOf ( Frontend::ExprId Use, Symbol Name ) const
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

void Volt::MiddleEnd::Analysis::TypeCheckerContext::WriteLocal ( Frontend::ExprId Use, Symbol Name, SemaTypeId Type )
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

std::span<const Volt::Core::Symbol> Volt::MiddleEnd::Analysis::TypeCheckerContext::Generics () const
{
    if ( SelfGenerics == nullptr )
    {
        return {};
    }
    return std::span<const Symbol>{ SelfGenerics->begin(), SelfGenerics->Size() };
}

std::string Volt::MiddleEnd::Analysis::TypeCheckerContext::NameOf ( NominalId Id ) const
{
    if ( not Id.IsValid() )
    {
        return "<unresolved>";
    }
    return std::string{ Ctx.Types.Text( Ctx.Types.Type( Id ).Name ) };
}

std::string Volt::MiddleEnd::Analysis::TypeCheckerContext::NameOfValue ( SemaTypeId Id ) const
{
    // The full generic signature, not the base nominal: two instantiations of
    // one generic differ only in their arguments, so dropping those turned
    // every such mismatch into "cannot assign G to G". `UnitTypes::Describe`
    // renders them from the canonical dictionary (TypeUniverse.hpp).
    return Ctx.Values.Describe( Ctx.Types, Id );
}

Volt::MiddleEnd::TypeSystem::SemaTypeId
Volt::MiddleEnd::Analysis::TypeCheckerContext::MakeType ( NominalId Base, Volt::Core::SmallVec<SemaTypeId, 2> Args )
{
    return Ctx.Values.Intern( SemaType{ .Base = Base, .Args = std::move( Args ) } );
}
