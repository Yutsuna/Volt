#include "Volt/Frontend/AST/PointFreeLowering.hpp"

#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/AST/Type.hpp"

#include <cstdio>
#include <optional>
#include <span>
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

[[nodiscard]] Frontend::ExprId BuildCompositionBody ( Frontend::AstContext &Context,
                                                      Frontend::ExprId CompId,
                                                      Frontend::ExprId ParamRef,
                                                      std::vector<Frontend::ExprId> &Consumed );

// Applies one step of a chain (a `Section`, a nested `Composition`, or an
// ordinary callee such as a named `def`) to `ArgExpr`.
//
// A `Section`/`Composition` step is inlined directly — its own body is built
// against `ArgExpr` in place, never through a `Call`— rather than wrapped in
// a `Lambda` and invoked. Every step this pass touches is applied to its
// argument exactly once, at the exact point this function returns to, so
// inlining duplicates no evaluation; it only removes indirection. This is
// what keeps `(&.to_string) >> (&.data) >> libc_puts` one flat expression,
// with one full-expression cleanup region, instead of three separately
// closure-lifted functions each finalizing their own temporaries before
// returning — which is what silently freed the `String` `&.to_string`
// allocated before `libc_puts` ever read the `Pointer<UInt8>` `&.data`
// aliased into it (a temporary is only safe past the call that produced it
// while it and its consumer share one region; splitting the chain across
// closures put them in different ones). An ordinary callee (a named
// function, or a hand-written closure value) is genuinely invoked instead —
// `Call{ Callee = StepId, Args = [ ArgExpr ] }`, exactly as before.
[[nodiscard]] Frontend::ExprId BuildStep ( Frontend::AstContext &Context,
                                           Frontend::ExprId StepId,
                                           Frontend::ExprId ArgExpr,
                                           std::vector<Frontend::ExprId> &Consumed )
{
    const Frontend::ExprKind Kind = Frontend::KindOf( Context.Expr( StepId ) );
    if ( Kind == Frontend::ExprKind::Section )
    {
        Consumed.push_back( StepId );
        return BuildSectionBody( Context, StepId, ArgExpr );
    }
    if ( Kind == Frontend::ExprKind::Composition )
    {
        Consumed.push_back( StepId );
        return BuildCompositionBody( Context, StepId, ArgExpr, Consumed );
    }

    Frontend::Call Cal;
    Cal.Loc    = Frontend::LocOf( Context.Expr( StepId ) );
    Cal.Callee = StepId;
    Cal.Args.PushBack( ArgExpr );
    Cal.ArgNames.PushBack( Core::Symbol{} );
    return Context.Add( Cal );
}

[[nodiscard]] Frontend::ExprId BuildCompositionBody ( Frontend::AstContext &Context,
                                                      Frontend::ExprId CompId,
                                                      Frontend::ExprId ParamRef,
                                                      std::vector<Frontend::ExprId> &Consumed )
{
    const Frontend::Composition Comp = std::get<Frontend::Composition>( Context.Expr( CompId ) );
    const Frontend::ExprId InnerExpr = BuildStep( Context, Comp.Lhs, ParamRef, Consumed );
    return BuildStep( Context, Comp.Rhs, InnerExpr, Consumed );
}

// A composition's result type is whatever its rightmost step returns.
// That rightmost step is the composition's own `Rhs` (parsed left-associative,
// so `Comp.Rhs` is already the last one applied); recurse through it when it is
// itself a nested Composition, and stop at a bare `Identifier` naming a
// top-level `def` in this same file, whose declared `-> ReturnType` is read back
// verbatim. When no explicit return type exists statically in the AST, returns
// nullopt so MiddleEnd (TypeBinder / ReinstantiateBody) deduces it dynamically
// from the body.
[[nodiscard]] std::optional<Frontend::TypeId> TryResolveChainReturnType ( Frontend::AstContext &Context, Frontend::ExprId Id )
{
    if ( const auto *Comp = std::get_if<Frontend::Composition>( &Context.Expr( Id ) ) )
    {
        return TryResolveChainReturnType( Context, Comp->Rhs );
    }

    if ( const auto *Sec = std::get_if<Frontend::Section>( &Context.Expr( Id ) ) )
    {
        if ( Sec->Kind == Frontend::ESectionKind::StaticCapture )
        {
            return TryResolveChainReturnType( Context, Sec->TargetExpr );
        }
        return std::nullopt;
    }

    const auto *Ident = std::get_if<Frontend::Identifier>( &Context.Expr( Id ) );
    if ( Ident == nullptr )
    {
        return std::nullopt;
    }

    auto FindInDecls = [&] ( auto &SelfRef, std::span<const Frontend::DeclId> Decls ) -> std::optional<Frontend::TypeId>
    {
        for ( const Frontend::DeclId DeclId : Decls )
        {
            if ( not DeclId.IsValid() )
            {
                continue;
            }
            const auto &Node = Context.Decl( DeclId );
            if ( const auto *Def = std::get_if<Frontend::Method>( &Node ) )
            {
                if ( Def->Name == Ident->Name and Def->ReturnType.IsValid() )
                {
                    return Def->ReturnType;
                }
            }
            else if ( const auto *Cls = std::get_if<Frontend::Class>( &Node ) )
            {
                if ( const auto Res = SelfRef( SelfRef, std::span{ Cls->Body.begin(), Cls->Body.end() } ) )
                {
                    return Res;
                }
            }
            else if ( const auto *Str = std::get_if<Frontend::Struct>( &Node ) )
            {
                if ( const auto Res = SelfRef( SelfRef, std::span{ Str->Body.begin(), Str->Body.end() } ) )
                {
                    return Res;
                }
            }
            else if ( const auto *Mix = std::get_if<Frontend::Mixin>( &Node ) )
            {
                if ( const auto Res = SelfRef( SelfRef, std::span{ Mix->Body.begin(), Mix->Body.end() } ) )
                {
                    return Res;
                }
            }
        }
        return std::nullopt;
    };

    return FindInDecls( FindInDecls, Context.TopDecls );
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

        const std::optional<TypeId> ResolvedReturnType = TryResolveChainReturnType( Context, ValId );

        const Core::Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );
        const Core::Symbol TSym      = Context.MakeUniqueSymbol( "T" );

        TypeRef ParamTRef;
        ParamTRef.Loc = Loc;
        ParamTRef.Path.PushBack( TSym );
        const TypeId ParamTId = Context.Add( ParamTRef );

        TypeId ReturnTId;
        if ( ResolvedReturnType.has_value() )
        {
            // The rightmost step's own declared return TypeId, reused as-is —
            // Type nodes are immutable after parsing, so sharing the Id
            // across two decls' signatures is safe.
            ReturnTId = *ResolvedReturnType;
        }

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

        // Every nested Section/Composition this recursion inlines (never
        // wraps in a Lambda) is collected here rather than left for Sema's
        // FunctionalLowering sweep to find — that sweep still scans the
        // whole arena by index regardless of reachability, so an orphaned
        // one left as-is would get needlessly closure-lifted into dead code
        // nobody calls, same reasoning as `ValId`/`AssignExprId` below.
        std::vector<ExprId> Consumed;
        const ExprId BodyExpr = ValueKind == ExprKind::Section ? BuildSectionBody( Context, ValId, ParamUseExpr )
                                                               : BuildCompositionBody( Context, ValId, ParamUseExpr, Consumed );

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
        for ( const ExprId ConsumedId : Consumed )
        {
            Context.Expr( ConsumedId ) = NilLiteral{ .Loc = Loc };
        }

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
