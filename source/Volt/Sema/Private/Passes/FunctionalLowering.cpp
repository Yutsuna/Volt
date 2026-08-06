#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Sema/Pass.hpp"

#include <variant>

namespace
{

using namespace Volt;

class FunctionalRewriter
{

public:

    explicit FunctionalRewriter ( Frontend::AstContext &InContext ) : Context( InContext )
    {
    }

    void Run ()
    {
        // Flat sweep over the arena, never a reflective walk: Add() reallocates
        // the arena storage, so a rewrite driven from a field reference inside a
        // live parent node writes into freed memory (see rules/ast-rewrite.md).
        // Sub-expressions are parsed first, so children carry smaller indices:
        // going 0 -> OriginalCount desugars the innermost node first, which is
        // what `(&.trim) >> (&.prefix("_"))` needs. Nodes created by a lowering
        // land past OriginalCount and are never Section/Composition, so leaving
        // them out of the sweep is intentional.
        const std::size_t OriginalCount = Context.ExprCount();

        for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            switch ( KindOf( Context.Expr( Id ) ) )
            {
            case Frontend::ExprKind::Section:
                Context.Expr( Id ) = LowerSection( Id );
                break;
            case Frontend::ExprKind::Composition:
                Context.Expr( Id ) = LowerComposition( Id );
                break;
            default:
                break;
            }
        }
    }

private:

    // Both lowerings copy their source node by value before the first Add(),
    // and return a node the caller stores back into the slot — the assignment
    // sequences the call before the left operand, so no Add() can invalidate
    // the destination.

    [[nodiscard]] Frontend::ExprNode LowerSection ( Frontend::ExprId SectionId )
    {
        const Frontend::Section Sec  = std::get<Frontend::Section>( Context.Expr( SectionId ) );
        const Core::Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );

        Frontend::Param Param;
        Param.Loc                   = Sec.Loc;
        Param.Name                  = ParamName;
        Param.DeclType              = Frontend::TypeId{};
        Param.Default               = Frontend::ExprId{};
        const Frontend::ParamId PId = Context.Add( Param );

        Frontend::ParamList Params;
        Params.PushBack( PId );

        Frontend::Identifier IdNode;
        IdNode.Loc                    = Sec.Loc;
        IdNode.Name                   = ParamName;
        const Frontend::ExprId IdExpr = Context.Add( IdNode );

        Frontend::ExprId BodyExpr{};

        switch ( Sec.Kind )
        {
        case Frontend::ESectionKind::InstanceMethod:
        {
            Frontend::Member Mem;
            Mem.Loc                    = Sec.Loc;
            Mem.Object                 = IdExpr;
            Mem.Name                   = Sec.Target;
            const Frontend::ExprId MId = Context.Add( Mem );

            Frontend::Call Cal;
            Cal.Loc    = Sec.Loc;
            Cal.Callee = MId;
            Cal.Args   = Sec.Args;
            for ( std::size_t Idx = 0; Idx < Sec.Args.Size(); ++Idx )
            {
                Cal.ArgNames.PushBack( Core::Symbol{} );
            }
            BodyExpr = Context.Add( Cal );
            break;
        }
        // An operator, unlike a named method, must desugar to a `Binary`/
        // `Unary` node rather than `Member`+`Call`: on a primitive receiver
        // MemberType deliberately records no resolution for a Binary/Unary's
        // own Member sub-node (rules/core-ast.md's operator contract — the
        // backend derives the instruction from the receiver's Primitive
        // spelling instead), and only Binary/Unary's own emitter knows that
        // convention. A `Call` node has no such fallback and expects every
        // callee to have resolved to something.
        case Frontend::ESectionKind::Operator:
        {
            if ( Sec.Args.Size() == 1 )
            {
                Frontend::Binary Bin;
                Bin.Loc  = Sec.Loc;
                Bin.Op   = Sec.Op;
                Bin.Lhs  = IdExpr;
                Bin.Rhs  = Sec.Args[0];
                BodyExpr = Context.Add( Bin );
            }
            else
            {
                Frontend::Unary Un;
                Un.Loc     = Sec.Loc;
                Un.Op      = Sec.Op;
                Un.Operand = IdExpr;
                BodyExpr   = Context.Add( Un );
            }
            break;
        }
        case Frontend::ESectionKind::StaticCapture:
        {
            Frontend::Call Cal;
            Cal.Loc    = Sec.Loc;
            Cal.Callee = Sec.TargetExpr;
            Cal.Args.PushBack( IdExpr );
            Cal.ArgNames.PushBack( Core::Symbol{} );
            BodyExpr = Context.Add( Cal );
            break;
        }
        }

        if ( Sec.bNegated )
        {
            Frontend::Unary Un;
            Un.Loc     = Sec.Loc;
            Un.Op      = Frontend::TokenKind::Bang;
            Un.Operand = BodyExpr;
            BodyExpr   = Context.Add( Un );
        }

        Frontend::Lambda Lam;
        Lam.Loc        = Sec.Loc;
        Lam.Params     = Params;
        Lam.ReturnType = Frontend::ExprId{};
        Lam.Body       = BodyExpr;
        return Lam;
    }

    [[nodiscard]] Frontend::ExprNode LowerComposition ( Frontend::ExprId CompId )
    {
        const Frontend::Composition Comp = std::get<Frontend::Composition>( Context.Expr( CompId ) );
        const Core::Symbol ParamName     = Context.MakeUniqueSymbol( "fn_tmp" );

        Frontend::Param Param;
        Param.Loc                   = Comp.Loc;
        Param.Name                  = ParamName;
        Param.DeclType              = Frontend::TypeId{};
        Param.Default               = Frontend::ExprId{};
        const Frontend::ParamId PId = Context.Add( Param );

        Frontend::ParamList Params;
        Params.PushBack( PId );

        Frontend::Identifier IdNode;
        IdNode.Loc                    = Comp.Loc;
        IdNode.Name                   = ParamName;
        const Frontend::ExprId IdExpr = Context.Add( IdNode );

        Frontend::Call InnerCall;
        InnerCall.Loc    = Comp.Loc;
        InnerCall.Callee = Comp.Lhs;
        InnerCall.Args.PushBack( IdExpr );
        InnerCall.ArgNames.PushBack( Core::Symbol{} );
        const Frontend::ExprId InnerExpr = Context.Add( InnerCall );

        Frontend::Call OuterCall;
        OuterCall.Loc    = Comp.Loc;
        OuterCall.Callee = Comp.Rhs;
        OuterCall.Args.PushBack( InnerExpr );
        OuterCall.ArgNames.PushBack( Core::Symbol{} );
        const Frontend::ExprId OuterExpr = Context.Add( OuterCall );

        Frontend::Lambda Lam;
        Lam.Loc        = Comp.Loc;
        Lam.Params     = Params;
        Lam.ReturnType = Frontend::ExprId{};
        Lam.Body       = OuterExpr;
        return Lam;
    }

    Frontend::AstContext &Context;
};

} // namespace

// Order 8 — rewrite Section and Composition nodes into standard Lambda nodes.
void Volt::Sema::FunctionalLowering ( Volt::Sema::PassContext &Context )
{
    FunctionalRewriter Rewriter{ Context.Ast };
    Rewriter.Run();
}
