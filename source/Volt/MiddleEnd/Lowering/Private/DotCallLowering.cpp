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

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <cstddef>
#include <variant>

namespace
{

using namespace Volt;

class DotCallRewriter
{

public:

    explicit DotCallRewriter ( Frontend::AstContext &InContext ) : Context( InContext )
    {
    }

    std::size_t Run ()
    {
        // Index sweep, copy-out / write-back — see rules/ast-rewrite.md.
        const std::size_t OriginalCount = Context.ExprCount();
        std::size_t Rewritten           = 0;

        for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            if ( KindOf( Context.Expr( Id ) ) == Frontend::ExprKind::DotCall )
            {
                Context.Expr( Id ) = LowerDotCall( Id );
                ++Rewritten;
            }
        }

        return Rewritten;
    }

private:

    [[nodiscard]] Frontend::ExprNode LowerDotCall ( Frontend::ExprId DotCallId )
    {
        const Frontend::DotCall Dot = std::get<Frontend::DotCall>( Context.Expr( DotCallId ) );

        Frontend::SelfExpr SelfNode;
        SelfNode.Loc                    = Dot.Loc;
        const Frontend::ExprId Receiver = Context.Add( Frontend::ExprNode{ SelfNode } );

        Frontend::Member Mem;
        Mem.Loc                       = Dot.Loc;
        Mem.Object                    = Receiver;
        Mem.Name                      = Dot.Method;
        const Frontend::ExprId Callee = Context.Add( Frontend::ExprNode{ Mem } );

        Frontend::Call Node;
        Node.Loc      = Dot.Loc;
        Node.Callee   = Callee;
        Node.Args     = Dot.Args;
        Node.ArgNames = Dot.ArgNames;
        return Node;
    }

    Frontend::AstContext &Context;
};

} // namespace

void Volt::MiddleEnd::Lowering::DotCallLowering ( Core::PassContext &Context )
{
    Context.Stats.DotCallsLowered += DotCallRewriter( Context.Ast ).Run();
}
