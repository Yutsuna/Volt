// DotCallLowering.cpp — order 23. Rewrites a leftover `.method( args )` into
// the explicit `self.method( args )` it means.
//
// Two constructs produce a DotCall, and both are already handled before this
// pass runs:
//
//   * `when .even?` inside a `case` — CaseLowering (order 22) rebinds it to the
//     scrutinee, hence this pass sits right after it and never steals one;
//   * a chain link on its own line (`users\n  .filter( ... )`) — the parser
//     folds it back into the chain, so it is a Member/Call, not a DotCall.
//
// What is left is a `.method` in statement position, which means "implicit
// self receiver". Lowering it here is what lets the backends ignore the node
// entirely, and it matches the choice CaseLowering already makes for a `case`
// with no target: an explicit SelfExpr, never a second resolution path.

#include "Volt/MiddleEnd/Lowering/LoweringRules.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <variant>

namespace
{

using namespace Volt;
using namespace Volt::MiddleEnd::Lowering;

// Every DotCall reaching order 23 is an implicit-self call, so this claims all
// of them unconditionally — the `Match` that returns true is the honest
// spelling of "there is no residual question here".
class DotCallToSelf
{

public:

    static constexpr Frontend::ExprKind Kind ()
    {
        return Frontend::ExprKind::DotCall;
    }

    bool Match ( RewriteContext & /*Context*/, Frontend::ExprId /*Id*/ ) const
    {
        return true;
    }

    Frontend::ExprNode Apply ( RewriteContext &Context, Frontend::ExprId Id ) const
    {
        // Copied out: both `Add()`s below can move the arena (ast-rewrite.md).
        const Frontend::DotCall Dot = std::get<Frontend::DotCall>( Context.Ast.Expr( Id ) );

        const Frontend::ExprId Receiver = Context.Ast.Add( Frontend::ExprNode{ Frontend::SelfExpr{ .Loc = Dot.Loc } } );
        const Frontend::ExprId Callee =
            Context.Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = Dot.Loc, .Object = Receiver, .Name = Dot.Method } } );

        Frontend::Call Lowered;
        Lowered.Loc      = Dot.Loc;
        Lowered.Callee   = Callee;
        Lowered.Args     = Dot.Args;
        Lowered.ArgNames = Dot.ArgNames;
        return Lowered;
    }
};

[[nodiscard]] const RuleRegistry &Rules ()
{
    static const RuleRegistry Registry = []
    {
        RuleRegistry Built;
        Built.Add<DotCallToSelf>( "DotCallToSelf" );
        return Built;
    }();
    return Registry;
}

} // namespace

void Volt::MiddleEnd::Lowering::DotCallLowering ( Core::PassContext &Context )
{
    RewriteContext Rewrite{ .Ast = Context.Ast, .Diags = Context.Diags };
    Context.Stats.DotCallsLowered += Rules().Run( Rewrite );
}
