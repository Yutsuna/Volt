#include "TypeCheckerContext.hpp"
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
    if ( const Binding *Bound = Ctx.Scopes.BindingOf( Use ) )
    {
        if ( const auto It = LocalTypes.find( Bound->Site ); It != LocalTypes.end() )
        {
            return It->second;
        }
    }
    if ( const auto It = Locals.find( Name ); It != Locals.end() )
    {
        return It->second;
    }
    return std::nullopt;
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
