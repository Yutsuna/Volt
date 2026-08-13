// LoweringRules.cpp — the one sweep every syntactic lowering shares.

#include "Volt/MiddleEnd/Lowering/LoweringRules.hpp"

#include <algorithm>
#include <cstddef>

void Volt::MiddleEnd::Lowering::RuleRegistry::Insert ( RewriteRule Rule, Frontend::ExprKind Kind )
{
    std::vector<RewriteRule> &Bucket = Buckets[static_cast<std::size_t>( Kind )];

    // Descending priority, insertion order preserved among equals — which is
    // what `std::stable_sort`'s guarantee buys, and why the insert point is
    // found rather than the bucket re-sorted.
    const auto At = std::ranges::find_if( Bucket, [&] ( const RewriteRule &Other ) { return Other.Priority < Rule.Priority; } );
    Bucket.insert( At, Rule );
}

std::size_t Volt::MiddleEnd::Lowering::RuleRegistry::Run ( RewriteContext &Context ) const
{
    // Index sweep, copy-out / write-back — see rules/ast-rewrite.md. The bound
    // is read once, before the first rewrite: `Apply` may `Add()` nodes, and
    // those must not be swept in the same pass that produced them.
    const std::size_t OriginalCount = Context.Ast.ExprCount();
    std::size_t Rewritten           = 0;

    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        // The only dispatch in the engine: one array index off the node's own
        // kind. A rule never sees a node it did not claim, so the cost of the
        // sweep is the cost of the rules that actually apply to this file.
        const std::vector<RewriteRule> &Bucket = Buckets[static_cast<std::size_t>( KindOf( Context.Ast.Expr( Id ) ) )];

        for ( const RewriteRule &Rule : Bucket )
        {
            if ( not Rule.Match( Context, Id ) )
            {
                continue;
            }
            // Apply reads the node and returns its replacement; the write-back
            // happens here so no rule can hold a reference across its own
            // `Add()` calls. First match wins — see Run's doc comment.
            Context.Ast.Expr( Id ) = Rule.Apply( Context, Id );
            ++Rewritten;
            break;
        }
    }

    return Rewritten;
}
