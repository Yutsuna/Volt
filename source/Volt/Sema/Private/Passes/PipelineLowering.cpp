#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Sema/Pass.hpp"

#include <variant>

namespace
{

using namespace Volt;

class PipelineRewriter
{

public:

    explicit PipelineRewriter ( Frontend::AstContext &InContext ) : Context( InContext )
    {
    }

    std::size_t Run ()
    {
        // Sub-expressions are parsed first, meaning children have smaller indices.
        // Iterating from 0 to OriginalCount - 1 ensures that in nested pipelines
        // (e.g. `5 |> add_three |> double`), the inner pipeline is desugared first.
        const std::size_t OriginalCount = Context.ExprCount();
        std::size_t Rewritten           = 0;

        for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            if ( KindOf( Context.Expr( Id ) ) == Frontend::ExprKind::Pipeline )
            {
                Context.Expr( Id ) = LowerPipeline( Id );
                ++Rewritten;
            }
        }
        return Rewritten;
    }

private:

    [[nodiscard]] Frontend::ExprNode LowerPipeline ( Frontend::ExprId PipelineId )
    {
        const Frontend::Pipeline Pipe     = std::get<Frontend::Pipeline>( Context.Expr( PipelineId ) );
        const Frontend::ExprNode &RhsNode = Context.Expr( Pipe.Rhs );

        Frontend::Call LoweredCall;

        if ( const auto *RhsCall = std::get_if<Frontend::Call>( &RhsNode ) )
        {
            // Case A: RHS is already a Call node
            // scale(3, 5) -> scale(Lhs, 3, 5)
            LoweredCall     = *RhsCall;
            LoweredCall.Loc = Pipe.Loc;

            // Prepend LHS to args and an empty symbol to arg names
            Frontend::ExprList NewArgs;
            NewArgs.PushBack( Pipe.Lhs );
            for ( Frontend::ExprId Arg : RhsCall->Args )
            {
                NewArgs.PushBack( Arg );
            }
            LoweredCall.Args = std::move( NewArgs );

            Frontend::SymbolList NewArgNames;
            NewArgNames.PushBack( Core::Symbol{} );
            for ( Core::Symbol Sym : RhsCall->ArgNames )
            {
                NewArgNames.PushBack( Sym );
            }
            LoweredCall.ArgNames = std::move( NewArgNames );
        }
        else
        {
            // Case B: RHS is a non-Call expression (Identifier, Member, etc.)
            // double -> double(Lhs)
            LoweredCall.Loc    = Pipe.Loc;
            LoweredCall.Callee = Pipe.Rhs;
            LoweredCall.Args.PushBack( Pipe.Lhs );
            LoweredCall.ArgNames.PushBack( Core::Symbol{} );
        }

        return LoweredCall;
    }

    Frontend::AstContext &Context;
};

} // namespace

// Order 8 — rewrite pipeline nodes into standard calls. Must run
// before ScopeResolver so it does not need to handle Pipeline nodes.
void Volt::MiddleEnd::Lowering::PipelineLowering ( Core::PassContext &Context )
{
    Context.Stats.PipelinesLowered += PipelineRewriter( Context.Ast ).Run();
}
