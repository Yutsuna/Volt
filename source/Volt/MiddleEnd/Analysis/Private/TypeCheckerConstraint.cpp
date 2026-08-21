#include "TypeCheckerConstraint.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Lowering/LoweringPasses.hpp"

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
inline void ConstrainChild ( Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                             Volt::Frontend::ExprId Child,
                             Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    static_cast<void>( Volt::MiddleEnd::Analysis::InferExpr( Self, Child ) );
    Self.ConstrainExprType( Child, TargetType );
}

// NOLINTBEGIN(readability-named-parameter, hicpp-named-parameter)
inline void ConstrainNode ( const auto &,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &,
                            Volt::Frontend::ExprId,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId )
{
}

inline void ConstrainNode ( const Volt::Frontend::Identifier &Node,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    // Only a local whose type came from an unconstrained initializer adopts
    // the expected type. One that was *written* (`big : UInt64`) keeps its own
    // and is checked against the context instead, by whichever of the five
    // sites called this — overwriting it here would make `-> Int32` accept a
    // trailing `big`, which is exactly the silence phase C exists to remove.
    const auto It = Self.UnconstrainedVarInitializers.find( Node.Name );
    if ( It != Self.UnconstrainedVarInitializers.end() )
    {
        const Volt::Frontend::ExprId InitExpr = It->second;
        Self.UnconstrainedVarInitializers.erase( It );
        Self.Ctx.Values.SetExprType( Expr, TargetType );
        Self.WriteLocal( Expr, Node.Name, TargetType );
        Self.ConstrainExprType( InitExpr, TargetType );
        return;
    }

    if ( Self.UnconstrainedParams.contains( Node.Name ) )
    {
        Self.UnconstrainedParams.erase( Node.Name );
        Self.Ctx.Values.SetExprType( Expr, TargetType );
        Self.WriteLocal( Expr, Node.Name, TargetType );
        return;
    }
}

inline void ConstrainNode ( const Volt::Frontend::Ternary &Node,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    ConstrainChild( Self, Node.Then, TargetType );
    ConstrainChild( Self, Node.Else, TargetType );
    Self.Ctx.Values.SetExprType( Expr, TargetType );
}

inline void ConstrainNode ( const Volt::Frontend::Binary &Node,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    // Resolve the operator on its own terms first — exactly what a bare visit
    // would do — before deciding whether the outer target has any business
    // reaching the operands. A self-typed operator (`Arithmetic#+`, `-`, ...)
    // returns its receiver's own type, so `doubled : Int32 = value * 2` needs
    // the target pushed through to settle `value` and `2`. A comparison
    // (`Comparable#==`, `<`, ...) returns a fixed `Bool` regardless of the
    // receiver, so pushing the target through it re-types the *operands* to
    // Bool instead of Bool being the operator's own answer — `check( a + b ==
    // 12.5 )` against a `Bool` parameter retyped `a + b` itself to Bool, and
    // the backend then emitted `icmp` on the float `FAdd` result. Comparing
    // the resolved result against the (independently resolved) receiver type
    // tells the two apart without hardcoding a single operator token.
    const Volt::MiddleEnd::TypeSystem::SemaTypeId Natural = Volt::MiddleEnd::Analysis::InferExpr( Self, Expr );
    if ( Natural.IsValid() and Natural != Self.Ctx.Values.ExprType( Node.Lhs ) )
    {
        return;
    }

    ConstrainChild( Self, Node.Lhs, TargetType );
    ConstrainChild( Self, Node.Rhs, TargetType );
    Self.Ctx.Values.SetExprType( Expr, TargetType );
}

// `neg : Int8 = -5` — `-5` is a `Unary` over the still-unconstrained literal
// `5`; without this overload it fell to the no-op default below and `5` kept
// its default `Int32`, so the assignment failed against `Int8`. Same shape as
// `Binary`, one operand.
inline void ConstrainNode ( const Volt::Frontend::Unary &Node,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    ConstrainChild( Self, Node.Operand, TargetType );
    Self.Ctx.Values.SetExprType( Expr, TargetType );
}

inline void ConstrainNode ( const Volt::Frontend::ArrayLit &Node,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    if ( not Self.Ctx.Values.Has( TargetType ) )
    {
        return;
    }

    const Volt::MiddleEnd::TypeSystem::SemaType &Target = Self.Ctx.Values.Get( TargetType );
    if ( not Target.Args.IsEmpty() )
    {
        for ( const Volt::Frontend::ExprId Elem : Node.Elements )
        {
            ConstrainChild( Self, Elem, Target.Args[0] );
        }
    }
}

inline void ConstrainNode ( const Volt::Frontend::HashLit &Node,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    if ( not Self.Ctx.Values.Has( TargetType ) )
    {
        return;
    }

    const Volt::MiddleEnd::TypeSystem::SemaType &Target = Self.Ctx.Values.Get( TargetType );
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

// A naked-type enum-case access with no payload to learn from
// (`Optional::None`) never gets its receiver's placeholder generic
// arguments (`PlaceholderTypeArgs`, `ExprInferencer.cpp`) filled by
// `UnifyArgs`: there is no call, hence no argument to unify against. The
// only remaining source of truth is the assignment's own declared type,
// exactly as `ArrayLit`'s empty `[]` adopts its target's element type above.
// Scoped to a resolution that is an `EnumCase` of the *same* nominal as
// `TargetType`, so an ordinary member access with a genuinely different
// type still gets a real mismatch diagnostic instead of a silent coercion.
inline void ConstrainNode ( const Volt::Frontend::Member &,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    const auto It = Self.CalleeResolution.find( Expr.Value );
    if ( It == Self.CalleeResolution.end() or It->second.Decl == nullptr or
         It->second.Decl->Kind != Volt::MiddleEnd::TypeSystem::EMemberKind::EnumCase )
    {
        return;
    }

    const Volt::MiddleEnd::TypeSystem::SemaTypeId Current = Self.Ctx.Values.ExprType( Expr );
    if ( not Current.IsValid() or not Self.Ctx.Values.Has( Current ) or not Self.Ctx.Values.Has( TargetType ) )
    {
        return;
    }
    if ( Self.Ctx.Values.Get( Current ).Base != Self.Ctx.Values.Get( TargetType ).Base )
    {
        return;
    }
    Self.Ctx.Values.SetExprType( Expr, TargetType );
}

inline void ConstrainNode ( const Volt::Frontend::Lambda &,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    const Volt::MiddleEnd::TypeSystem::SemaTypeId OuterExpected = Self.ExpectedClosure;
    Self.ExpectedClosure                                        = TargetType;
    // Re-infer under the constraint: BindClosureParams will consume
    // ExpectedClosure and fill the parameter types from the target's slots.
    static_cast<void>( Volt::MiddleEnd::Analysis::InferExpr( Self, Expr ) );
    Self.ExpectedClosure = OuterExpected;
}

inline void ConstrainNode ( const Volt::Frontend::Block &,
                            Volt::MiddleEnd::Analysis::TypeCheckerContext &Self,
                            Volt::Frontend::ExprId Expr,
                            Volt::MiddleEnd::TypeSystem::SemaTypeId TargetType )
{
    Self.Ctx.Values.SetExprType( Expr, TargetType );
    const Volt::MiddleEnd::TypeSystem::SemaTypeId OuterExpected = Self.ExpectedClosure;
    Self.ExpectedClosure                                        = TargetType;
    static_cast<void>( Volt::MiddleEnd::Analysis::InferExpr( Self, Expr ) );
    Self.ExpectedClosure = OuterExpected;
}

// NOLINTEND(readability-named-parameter, hicpp-named-parameter)

} // namespace

/**
 * Public
 */

void Volt::MiddleEnd::Analysis::TypeCheckerContext::ConstrainExprType ( Frontend::ExprId Expr, SemaTypeId TargetType )
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
