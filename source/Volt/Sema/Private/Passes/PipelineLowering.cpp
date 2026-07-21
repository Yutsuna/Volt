#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Sema/Pass.hpp"

#include <variant>

namespace Volt
{

namespace Sema
{

    namespace
    {

        using namespace Frontend;

        class PipelineRewriter
        {

        public:

            explicit PipelineRewriter ( AstContext &InContext ) : Context( InContext )
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
                    const ExprId Id{ static_cast<ExprId::ValueType>( Index ) };
                    if ( KindOf( Context.Expr( Id ) ) == ExprKind::Pipeline )
                    {
                        Context.Expr( Id ) = LowerPipeline( Id );
                        ++Rewritten;
                    }
                }
                return Rewritten;
            }

        private:

            [[nodiscard]] ExprNode LowerPipeline ( ExprId PipelineId )
            {
                const Pipeline Pipe     = std::get<Pipeline>( Context.Expr( PipelineId ) );
                const ExprNode &RhsNode = Context.Expr( Pipe.Rhs );

                Call LoweredCall;

                if ( const auto *RhsCall = std::get_if<Call>( &RhsNode ) )
                {
                    // Case A: RHS is already a Call node
                    // scale(3, 5) -> scale(Lhs, 3, 5)
                    LoweredCall     = *RhsCall;
                    LoweredCall.Loc = Pipe.Loc;

                    // Prepend LHS to args and an empty symbol to arg names
                    ExprList NewArgs;
                    NewArgs.PushBack( Pipe.Lhs );
                    for ( ExprId Arg : RhsCall->Args )
                    {
                        NewArgs.PushBack( Arg );
                    }
                    LoweredCall.Args = std::move( NewArgs );

                    SymbolList NewArgNames;
                    NewArgNames.PushBack( Symbol{} );
                    for ( Symbol Sym : RhsCall->ArgNames )
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
                    LoweredCall.ArgNames.PushBack( Symbol{} );
                }

                return LoweredCall;
            }

            AstContext &Context;
        };

    } // namespace

    // Order 8 — rewrite pipeline nodes into standard calls. Must run
    // before ScopeResolver so it does not need to handle Pipeline nodes.
    void PipelineLowering ( PassContext &Context )
    {
        Context.Stats.PipelinesLowered += PipelineRewriter( Context.Ast ).Run();
    }

} // namespace Sema

} // namespace Volt
