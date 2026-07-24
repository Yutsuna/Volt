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

    if ( const Binding *Bound = Ctx.Scopes.BindingOf( Use ) )
    {
        if ( const auto It = LocalTypes.find( Bound->Site ); It != LocalTypes.end() )
        {
            FoundType = It->second;
        }
    }
    if ( not FoundType.IsValid() )
    {
        if ( const auto It = Locals.find( Name ); It != Locals.end() )
        {
            FoundType = It->second;
        }
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

void Volt::Sema::TypeCheckerPass::TypeCheckerContext::WriteLocal ( Frontend::ExprId Use, Symbol Name, SemaTypeId Type )
{
    if ( const Binding *Bound = Ctx.Scopes.BindingOf( Use ) )
    {
        LocalTypes[Bound->Site] = Type;
        Ctx.Values.SetSiteType( Bound->Site, Type );
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
