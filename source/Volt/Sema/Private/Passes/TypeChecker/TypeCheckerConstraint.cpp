#include "TypeCheckerConstraint.hpp"
#include "TypeCheckerContext.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

/**
 * Private
 */

namespace
{

// NOLINTBEGIN(readability-named-parameter, hicpp-named-parameter)
inline void
ConstrainNode ( const auto &, Volt::Sema::TypeCheckerPass::TypeCheckerContext &, Volt::Frontend::ExprId, Volt::Sema::SemaTypeId )
{
}

inline void ConstrainNode ( const Volt::Frontend::Identifier &Node,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    if ( const auto It = Self.UnconstrainedVarInitializers.find( Node.Name ); It != Self.UnconstrainedVarInitializers.end() )
    {
        const Volt::Frontend::ExprId InitExpr = It->second;
        Self.UnconstrainedVarInitializers.erase( It );
        Self.WriteLocal( Expr, Node.Name, TargetType );
        Self.ConstrainExprType( InitExpr, TargetType );
    }
}

inline void ConstrainNode ( const Volt::Frontend::Ternary &Node,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    Self.ConstrainExprType( Node.Then, TargetType );
    Self.ConstrainExprType( Node.Else, TargetType );
    Self.Ctx.Values.SetExprType( Expr, TargetType );
}

inline void ConstrainNode ( const Volt::Frontend::Binary &Node,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    Self.ConstrainExprType( Node.Lhs, TargetType );
    Self.ConstrainExprType( Node.Rhs, TargetType );
    Self.Ctx.Values.SetExprType( Expr, TargetType );
}

inline void ConstrainNode ( const Volt::Frontend::ArrayLit &Node,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    if ( not Self.Ctx.Values.Has( TargetType ) )
    {
        return;
    }

    const Volt::Sema::SemaType &Target = Self.Ctx.Values.Get( TargetType );
    if ( not Target.Args.IsEmpty() )
    {
        for ( const Volt::Frontend::ExprId Elem : Node.Elements )
        {
            Self.ConstrainExprType( Elem, Target.Args[0] );
        }
    }
}

inline void ConstrainNode ( const Volt::Frontend::HashLit &Node,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    if ( not Self.Ctx.Values.Has( TargetType ) )
    {
        return;
    }

    const Volt::Sema::SemaType &Target = Self.Ctx.Values.Get( TargetType );
    if ( Target.Args.Size() >= 2 )
    {
        for ( const Volt::Frontend::ExprId Key : Node.Keys )
        {
            Self.ConstrainExprType( Key, Target.Args[0] );
        }
        for ( const Volt::Frontend::ExprId Value : Node.Values )
        {
            Self.ConstrainExprType( Value, Target.Args[1] );
        }
    }
}

inline void ConstrainNode ( const Volt::Frontend::Lambda &,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    const Volt::Sema::SemaTypeId OuterExpected = Self.ExpectedClosure;
    Self.ExpectedClosure                       = TargetType;
    // Re-infer under the constraint: BindClosureParams will consume
    // ExpectedClosure and fill the parameter types from the target's slots.
    static_cast<void>( Volt::Sema::TypeCheckerPass::InferExpr( Self, Expr ) );
    Self.ExpectedClosure = OuterExpected;
}

inline void ConstrainNode ( const Volt::Frontend::Block &,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    const Volt::Sema::SemaTypeId OuterExpected = Self.ExpectedClosure;
    Self.ExpectedClosure                       = TargetType;
    static_cast<void>( Volt::Sema::TypeCheckerPass::InferExpr( Self, Expr ) );
    Self.ExpectedClosure = OuterExpected;
}

// NOLINTEND(readability-named-parameter, hicpp-named-parameter)

} // namespace

/**
 * Public
 */

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

    std::visit( [this, Expr, TargetType] ( const auto &ConcreteNode ) { ConstrainNode( ConcreteNode, *this, Expr, TargetType ); },
                Node );
}
