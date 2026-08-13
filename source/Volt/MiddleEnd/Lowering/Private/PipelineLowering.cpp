// PipelineLowering.cpp — order 9. `a |> f` becomes `f( a )`.
//
// Two rules rather than one with a branch: what `|>` does depends entirely on
// whether its right-hand side is already a call, and that is a question about
// *which rule applies*, not a step inside one. `RuleRegistry` picks by
// priority and first match, so each rule below is a single unconditional
// transformation — see LoweringRules.hpp.

#include "Volt/MiddleEnd/Lowering/LoweringRules.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <variant>

namespace
{

using namespace Volt;
using namespace Volt::MiddleEnd::Lowering;

[[nodiscard]] const Frontend::Pipeline &PipeAt ( RewriteContext &Context, Frontend::ExprId Id )
{
    return std::get<Frontend::Pipeline>( Context.Ast.Expr( Id ) );
}

// `Head` followed by `Tail` — SmallVec grows only at the back, and a pipe
// prepends by definition.
template <typename List> [[nodiscard]] List PrependedWith ( const typename List::ValueType &Head, const List &Tail )
{
    List Joined;
    Joined.PushBack( Head );
    for ( const auto &Element : Tail )
    {
        Joined.PushBack( Element );
    }
    return Joined;
}

// `5 |> scale( 3 )` → `scale( 5, 3 )`. The piped value joins the arguments
// already written, at the front, with the empty name a positional argument
// carries.
class PipeIntoCall
{

public:

    static constexpr Frontend::ExprKind Kind ()
    {
        return Frontend::ExprKind::Pipeline;
    }

    bool Match ( RewriteContext &Context, Frontend::ExprId Id ) const
    {
        return std::holds_alternative<Frontend::Call>( Context.Ast.Expr( PipeAt( Context, Id ).Rhs ) );
    }

    Frontend::ExprNode Apply ( RewriteContext &Context, Frontend::ExprId Id ) const
    {
        const Frontend::Pipeline Pipe = PipeAt( Context, Id );
        Frontend::Call Lowered        = std::get<Frontend::Call>( Context.Ast.Expr( Pipe.Rhs ) );

        Lowered.Loc      = Pipe.Loc;
        Lowered.Args     = PrependedWith( Pipe.Lhs, Lowered.Args );
        Lowered.ArgNames = PrependedWith( Volt::Core::Symbol{}, Lowered.ArgNames );
        return Lowered;
    }
};

// `5 |> double` → `double( 5 )`. The right-hand side names the callee and the
// piped value is the whole argument list.
class PipeIntoValue
{

public:

    static constexpr Frontend::ExprKind Kind ()
    {
        return Frontend::ExprKind::Pipeline;
    }

    bool Match ( RewriteContext & /*Context*/, Frontend::ExprId /*Id*/ ) const
    {
        return true;
    }

    Frontend::ExprNode Apply ( RewriteContext &Context, Frontend::ExprId Id ) const
    {
        const Frontend::Pipeline Pipe = PipeAt( Context, Id );

        Frontend::Call Lowered;
        Lowered.Loc    = Pipe.Loc;
        Lowered.Callee = Pipe.Rhs;
        Lowered.Args.PushBack( Pipe.Lhs );
        Lowered.ArgNames.PushBack( Volt::Core::Symbol{} );
        return Lowered;
    }
};

[[nodiscard]] const RuleRegistry &Rules ()
{
    static const RuleRegistry Registry = []
    {
        RuleRegistry Built;
        // PipeIntoCall first: it is the special case, and PipeIntoValue
        // matches everything.
        Built.Add<PipeIntoCall>( "PipeIntoCall", 10 ).Add<PipeIntoValue>( "PipeIntoValue", 0 );
        return Built;
    }();
    return Registry;
}

} // namespace

// Order 9 — rewrite pipeline nodes into standard calls. Must run before
// ScopeResolver so it does not need to handle Pipeline nodes.
void Volt::MiddleEnd::Lowering::PipelineLowering ( Core::PassContext &Context )
{
    RewriteContext Rewrite{ .Ast = Context.Ast, .Diags = Context.Diags };
    Context.Stats.PipelinesLowered += Rules().Run( Rewrite );
}
