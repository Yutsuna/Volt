#include "TypeCheckerContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"

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

void Volt::Sema::TypeCheckerPass::TypeCheckerContext::ConstrainExprType ( Frontend::ExprId Expr, SemaTypeId TargetType )
{
    if ( not Expr.IsValid() or not TargetType.IsValid() )
    {
        return;
    }

    if ( UnconstrainedLiterals.contains( Expr.Value ) )
    {
        Ctx.Values.SetExprType( Expr, TargetType );
        UnconstrainedLiterals.erase( Expr.Value );
        return;
    }

    const auto &Node = Ctx.Ast.Expr( Expr );

    if ( const auto *IdNode = std::get_if<Frontend::Identifier>( &Node ) )
    {
        Ctx.Values.SetExprType( Expr, TargetType );
        if ( const auto It = UnconstrainedVarInitializers.find( IdNode->Name ); It != UnconstrainedVarInitializers.end() )
        {
            const Frontend::ExprId InitExpr = It->second;
            UnconstrainedVarInitializers.erase( It );
            WriteLocal( Expr, IdNode->Name, TargetType );
            ConstrainExprType( InitExpr, TargetType );
        }
        return;
    }

    if ( const auto *Ternary = std::get_if<Frontend::Ternary>( &Node ) )
    {
        ConstrainExprType( Ternary->Then, TargetType );
        ConstrainExprType( Ternary->Else, TargetType );
        Ctx.Values.SetExprType( Expr, TargetType );
        return;
    }

    if ( const auto *Binary = std::get_if<Frontend::Binary>( &Node ) )
    {
        ConstrainExprType( Binary->Lhs, TargetType );
        ConstrainExprType( Binary->Rhs, TargetType );
        Ctx.Values.SetExprType( Expr, TargetType );
        return;
    }
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
