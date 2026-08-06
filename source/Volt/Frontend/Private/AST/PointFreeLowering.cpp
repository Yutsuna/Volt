#include "Volt/Frontend/AST/PointFreeLowering.hpp"

#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/AST/Type.hpp"

#include <cstdio>
#include <variant>
#include <vector>

namespace
{

using namespace Volt;

// Same desugar `FunctionalLowering` (Sema, Order 8) applies to every other
// Section/Composition: build the `fn_tmp => ...` body. Kept local rather
// than shared across the Frontend/Sema boundary — the two callers diverge
// immediately after (a Lambda literal here vs. a generic top-level `def`),
// and the shared part is small.
[[nodiscard]] Frontend::ExprId
BuildSectionBody ( Frontend::AstContext &Context, Frontend::ExprId SectionId, Frontend::ExprId ParamRef )
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
            Cal.ArgNames.PushBack( Core::Symbol{} );
        }
        BodyExpr = Context.Add( Cal );
        break;
    }
    // A Binary/Unary node, never Member+Call: on a primitive receiver
    // MemberType leaves a Binary/Unary's own Member sub-node without a
    // resolution by design (rules/core-ast.md's operator contract), and
    // only the Binary/Unary emitter knows to fall back to the receiver's
    // Primitive spelling — a Call node has no such fallback.
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

    return BodyExpr;
}

[[nodiscard]] Frontend::ExprId
BuildCompositionBody ( Frontend::AstContext &Context, Frontend::ExprId CompId, Frontend::ExprId ParamRef )
{
    const Frontend::Composition Comp = std::get<Frontend::Composition>( Context.Expr( CompId ) );

    Frontend::Call InnerCall;
    InnerCall.Loc    = Comp.Loc;
    InnerCall.Callee = Comp.Lhs;
    InnerCall.Args.PushBack( ParamRef );
    InnerCall.ArgNames.PushBack( Core::Symbol{} );
    const Frontend::ExprId InnerExpr = Context.Add( InnerCall );

    Frontend::Call OuterCall;
    OuterCall.Loc    = Comp.Loc;
    OuterCall.Callee = Comp.Rhs;
    OuterCall.Args.PushBack( InnerExpr );
    OuterCall.ArgNames.PushBack( Core::Symbol{} );
    return Context.Add( OuterCall );
}

} // namespace

void Volt::Frontend::LowerPointFreeDefs ( AstContext &Context )
{
    std::vector<StmtId> Keep;
    Keep.reserve( Context.TopStmts.size() );

    for ( const StmtId Id : Context.TopStmts )
    {
        const auto *Es  = std::get_if<ExprStmt>( &Context.Stmt( Id ) );
        const auto *Asn = Es != nullptr ? std::get_if<Assign>( &Context.Expr( Es->Expr ) ) : nullptr;
        const bool bBareTarget =
            Asn != nullptr and Asn->Op == TokenKind::Assign and std::holds_alternative<Identifier>( Context.Expr( Asn->Target ) );
        const ExprKind ValueKind = bBareTarget ? KindOf( Context.Expr( Asn->Value ) ) : ExprKind::None;

        if ( ValueKind != ExprKind::Section and ValueKind != ExprKind::Composition )
        {
            Keep.push_back( Id );
            continue;
        }

        // Copied out before any Add() below can reallocate the arenas Es/Asn
        // point into (rules/ast-rewrite.md) — Es lives in the Stmt arena,
        // Asn in the Expr arena, and the loop body adds to both.
        const Core::Symbol Name     = std::get<Identifier>( Context.Expr( Asn->Target ) ).Name;
        const ExprId ValId          = Asn->Value;
        const ExprId AssignExprId   = Es->Expr;
        const Core::SourceRange Loc = ValueKind == ExprKind::Section ? std::get<Section>( Context.Expr( ValId ) ).Loc
                                                                     : std::get<Composition>( Context.Expr( ValId ) ).Loc;

        const Core::Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );
        const Core::Symbol TSym      = Context.MakeUniqueSymbol( "T" );

        TypeRef ParamTRef;
        ParamTRef.Loc = Loc;
        ParamTRef.Path.PushBack( TSym );
        const TypeId ParamTId = Context.Add( ParamTRef );

        TypeRef ReturnTRef;
        ReturnTRef.Loc = Loc;
        ReturnTRef.Path.PushBack( TSym );
        const TypeId ReturnTId = Context.Add( ReturnTRef );

        Param ParamNode;
        ParamNode.Loc          = Loc;
        ParamNode.Name         = ParamName;
        ParamNode.DeclType     = ParamTId;
        ParamNode.Default      = ExprId{};
        const ParamId ParamRef = Context.Add( ParamNode );

        Identifier ParamUse;
        ParamUse.Loc              = Loc;
        ParamUse.Name             = ParamName;
        const ExprId ParamUseExpr = Context.Add( ParamUse );

        const ExprId BodyExpr = ValueKind == ExprKind::Section ? BuildSectionBody( Context, ValId, ParamUseExpr )
                                                               : BuildCompositionBody( Context, ValId, ParamUseExpr );

        // `ValId` and `AssignExprId` are dead weight now — nothing will
        // reference either again once the Assign is dropped from TopStmts
        // below. But they are still live *slots* in the flat Expr arena:
        // AstInvariant's own value-position collection walks every `Assign`
        // node by index regardless of reachability, and the ordinary Sema
        // FunctionalLowering sweep does the same for Section/Composition —
        // leaving either slot as-is would get it rewritten/flagged as an
        // orphan nothing ever types, exactly as if this pass never ran.
        Context.Expr( ValId )        = NilLiteral{ .Loc = Loc };
        Context.Expr( AssignExprId ) = NilLiteral{ .Loc = Loc };

        ExprStmt TailStmt;
        TailStmt.Loc  = Loc;
        TailStmt.Expr = BodyExpr;

        Method Def;
        Def.Loc        = Loc;
        Def.Name       = Name;
        Def.ReturnType = ReturnTId;
        Def.Generics.PushBack( TSym );
        Def.Params.PushBack( ParamRef );
        Def.Body.PushBack( Context.Add( TailStmt ) );
        Context.TopDecls.push_back( Context.Add( Def ) );

        // The original `Assign`/`ExprStmt` are dead weight now — dropped
        // from TopStmts rather than kept as a no-op, so ScopeResolver never
        // sees `Name` bound as a local/global on top of the Method above.
    }

    Context.TopStmts = std::move( Keep );
}
