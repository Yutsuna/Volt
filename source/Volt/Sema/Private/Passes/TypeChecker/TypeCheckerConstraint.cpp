#include "TypeCheckerConstraint.hpp"
#include "TypeCheckerContext.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

/**
 * Private
 */

namespace
{

// Infer a child, then push the expectation into it. The inference is what
// makes the second half work at all: a constraint applied *before* the
// expression tree has been walked — which is what every one of the five
// assignment sites does, so that a closure initialiser gets its
// ExpectedClosure — stamps the parent through SetExprType, and InferExpr
// memoises on exactly that stamp. The parent then never gets walked, and its
// operands stay untyped forever. `doubled : Int32 = value * 2` typed the
// Binary and left both `value` and `2` with no type at all.
inline void ConstrainChild ( Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                             Volt::Frontend::ExprId Child,
                             Volt::Sema::SemaTypeId TargetType )
{
    static_cast<void>( Volt::Sema::TypeCheckerPass::InferExpr( Self, Child ) );
    Self.ConstrainExprType( Child, TargetType );
}

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
    // Only a local whose type came from an unconstrained initializer adopts
    // the expected type. One that was *written* (`big : UInt64`) keeps its own
    // and is checked against the context instead, by whichever of the five
    // sites called this — overwriting it here would make `-> Int32` accept a
    // trailing `big`, which is exactly the silence phase C exists to remove.
    const auto It = Self.UnconstrainedVarInitializers.find( Node.Name );
    if ( It == Self.UnconstrainedVarInitializers.end() )
    {
        return;
    }

    const Volt::Frontend::ExprId InitExpr = It->second;
    Self.UnconstrainedVarInitializers.erase( It );
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    Self.WriteLocal( Expr, Node.Name, TargetType );
    Self.ConstrainExprType( InitExpr, TargetType );
}

inline void ConstrainNode ( const Volt::Frontend::Ternary &Node,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    ConstrainChild( Self, Node.Then, TargetType );
    ConstrainChild( Self, Node.Else, TargetType );
    Self.Ctx.Values.SetExprType( Expr, TargetType );
}

inline void ConstrainNode ( const Volt::Frontend::Binary &Node,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    ConstrainChild( Self, Node.Lhs, TargetType );
    ConstrainChild( Self, Node.Rhs, TargetType );
    Self.Ctx.Values.SetExprType( Expr, TargetType );
}

// `neg : Int8 = -5` — `-5` is a `Unary` over the still-unconstrained literal
// `5`; without this overload it fell to the no-op default below and `5` kept
// its default `Int32`, so the assignment failed against `Int8`. Same shape as
// `Binary`, one operand.
inline void ConstrainNode ( const Volt::Frontend::Unary &Node,
                            Volt::Sema::TypeCheckerPass::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::Sema::SemaTypeId TargetType )
{
    ConstrainChild( Self, Node.Operand, TargetType );
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
            ConstrainChild( Self, Elem, Target.Args[0] );
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
            ConstrainChild( Self, Key, Target.Args[0] );
        }
        for ( const Volt::Frontend::ExprId Value : Node.Values )
        {
            ConstrainChild( Self, Value, Target.Args[1] );
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
