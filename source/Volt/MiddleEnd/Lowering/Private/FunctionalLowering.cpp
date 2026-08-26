#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <unordered_set>
#include <variant>
#include <vector>

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

        for ( std::size_t Index = OriginalCount; Index > 0; --Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index - 1 ) };
            if ( ConsumedNodes.contains( Id.Value ) )
            {
                continue;
            }

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

    std::unordered_set<Frontend::ExprId::ValueType> ConsumedNodes;

    [[nodiscard]] Frontend::ExprId BuildSectionBody ( Frontend::ExprId SectionId, Frontend::ExprId ParamRef )
    {
        const Frontend::Section Sec = std::get<Frontend::Section>( Context.Expr( SectionId ) );
        Frontend::ExprId BodyExpr{};

        switch ( Sec.Kind )
        {
        case Frontend::ESectionKind::InstanceMethod:
        {
            Frontend::Member Mem;
            Mem.Loc                    = Sec.Loc;
            Mem.Object                 = ParamRef;
            Mem.Name                   = Sec.Target;
            const Frontend::ExprId MId = Context.Add( Mem );

            Frontend::Call Cal;
            Cal.Loc    = Sec.Loc;
            Cal.Callee = MId;
            Cal.Args   = Sec.Args;
            for ( std::size_t Idx = 0; Idx < Sec.Args.Size(); ++Idx )
            {
                Cal.ArgNames.PushBack( Volt::Core::Symbol{} );
            }
            BodyExpr = Context.Add( Cal );
            break;
        }
        case Frontend::ESectionKind::Operator:
        {
            if ( Sec.Args.Size() == 1 )
            {
                Frontend::Binary Bin;
                Bin.Loc  = Sec.Loc;
                Bin.Op   = Sec.Op;
                Bin.Lhs  = ParamRef;
                Bin.Rhs  = Sec.Args[0];
                BodyExpr = Context.Add( Bin );
            }
            else
            {
                Frontend::Unary Un;
                Un.Loc     = Sec.Loc;
                Un.Op      = Sec.Op;
                Un.Operand = ParamRef;
                BodyExpr   = Context.Add( Un );
            }
            break;
        }
        case Frontend::ESectionKind::StaticCapture:
        {
            Frontend::Call Cal;
            Cal.Loc    = Sec.Loc;
            Cal.Callee = Sec.TargetExpr;
            Cal.Args.PushBack( ParamRef );
            Cal.ArgNames.PushBack( Volt::Core::Symbol{} );
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

        return BodyExpr;
    }

    [[nodiscard]] Frontend::ExprId BuildStep ( Frontend::ExprId StepId, Frontend::ExprId ArgExpr )
    {
        const Frontend::ExprKind Kind = Frontend::KindOf( Context.Expr( StepId ) );
        if ( Kind == Frontend::ExprKind::Section )
        {
            ConsumedNodes.insert( StepId.Value );
            const Frontend::ExprId Res = BuildSectionBody( StepId, ArgExpr );
            Context.Expr( StepId )     = Frontend::NilLiteral{ .Loc = Frontend::LocOf( Context.Expr( StepId ) ) };
            return Res;
        }
        if ( Kind == Frontend::ExprKind::Composition )
        {
            ConsumedNodes.insert( StepId.Value );
            const Frontend::ExprId Res = BuildCompositionBody( StepId, ArgExpr );
            Context.Expr( StepId )     = Frontend::NilLiteral{ .Loc = Frontend::LocOf( Context.Expr( StepId ) ) };
            return Res;
        }

        Frontend::Call Cal;
        Cal.Loc    = Frontend::LocOf( Context.Expr( StepId ) );
        Cal.Callee = StepId;
        Cal.Args.PushBack( ArgExpr );
        Cal.ArgNames.PushBack( Volt::Core::Symbol{} );
        return Context.Add( Cal );
    }

    [[nodiscard]] Frontend::ExprId BuildCompositionBody ( Frontend::ExprId CompId, Frontend::ExprId ParamRef )
    {
        const Frontend::Composition Comp = std::get<Frontend::Composition>( Context.Expr( CompId ) );
        const Frontend::ExprId InnerExpr = BuildStep( Comp.Lhs, ParamRef );
        return BuildStep( Comp.Rhs, InnerExpr );
    }

    [[nodiscard]] Frontend::ExprNode LowerSection ( Frontend::ExprId SectionId )
    {
        const Frontend::Section Sec        = std::get<Frontend::Section>( Context.Expr( SectionId ) );
        const Volt::Core::Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );

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

        const Frontend::ExprId BodyExpr = BuildSectionBody( SectionId, IdExpr );

        Frontend::Lambda Lam;
        Lam.Loc        = Sec.Loc;
        Lam.Params     = Params;
        Lam.ReturnType = Frontend::ExprId{};
        Lam.Body       = BodyExpr;
        return Lam;
    }

    [[nodiscard]] Frontend::ExprNode LowerComposition ( Frontend::ExprId CompId )
    {
        const Frontend::Composition Comp   = std::get<Frontend::Composition>( Context.Expr( CompId ) );
        const Volt::Core::Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );

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

        const Frontend::ExprId BodyExpr = BuildCompositionBody( CompId, IdExpr );

        Frontend::Lambda Lam;
        Lam.Loc        = Comp.Loc;
        Lam.Params     = Params;
        Lam.ReturnType = Frontend::ExprId{};
        Lam.Body       = BodyExpr;
        return Lam;
    }

    Frontend::AstContext &Context;
};

} // namespace

// Order 8 — rewrite Section and Composition nodes into standard Lambda nodes.
void Volt::MiddleEnd::Lowering::FunctionalLowering ( Core::PassContext &Context )
{
    FunctionalRewriter Rewriter{ Context.Ast };
    Rewriter.Run();
}
