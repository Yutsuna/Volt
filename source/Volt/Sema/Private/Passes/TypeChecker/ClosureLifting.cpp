#include "ClosureLifting.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Layout/ClosureFrame.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <vector>

namespace
{

using namespace Volt;
using namespace Volt::Sema::TypeCheckerPass;

// `Proc<R>#code`'s own already-resolved field type — Pointer<UInt8> — read
// off the declaration rather than reconstructed from a byte-width node-kind
// claim nothing declares as "UInt8" specifically (rules/zero-hardcode.md;
// the same fix ExprInferencer's FuncAddr arm already applies).
Sema::SemaTypeId BytePointerType ( Sema::TypeCheckerPass::TypeCheckerContext &Context )
{
    const auto FuncBase = Context.Ctx.Types.LookupNodeKind( "FuncType" );
    if ( not FuncBase )
    {
        return Sema::SemaTypeId{};
    }
    const auto CodeField = Context.Ctx.Types.LookupMember( *FuncBase, "code" );
    if ( CodeField.Decl == nullptr )
    {
        return Sema::SemaTypeId{};
    }
    return Sema::Instantiate( Context.Ctx.Types, CodeField.Decl->Result, {}, Sema::SemaTypeId{}, Context.Ctx.Values );
}

// A naked-type receiver expr, hand-stamped to `Type` — the same technique
// LowerArrayLit's own `Object` child uses for `T.new()`.
Frontend::ExprId NakedTypeExpr ( TypeCheckerContext &Context, Sema::NominalId Base, Sema::SemaTypeId Type )
{
    Frontend::AstContext &Ast       = Context.Ctx.Ast;
    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Base ).Name );
    const Frontend::ExprId ObjectId =
        Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = Ast.Strings().Intern( NameText ) } } );
    Context.Ctx.Values.SetExprType( ObjectId, Type );
    Context.NakedTypeExprs.insert( ObjectId.Value );
    return ObjectId;
}

// `Receiver.Name( Args... )`, inferred immediately — ordinary member
// resolution off an already-typed receiver, exactly like LowerStringLit's
// constructor call.
Frontend::ExprId
CallMember ( TypeCheckerContext &Context, Frontend::ExprId Receiver, std::string_view Name, const Frontend::ExprList &Args )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;
    const Frontend::ExprId MemberId =
        Ast.Add( Frontend::ExprNode{ Frontend::Member{ .Loc = {}, .Object = Receiver, .Name = Ast.Strings().Intern( Name ) } } );
    Frontend::SymbolList ArgNames;
    for ( std::size_t Index = 0; Index < Args.Size(); ++Index )
    {
        ArgNames.PushBack( Core::Symbol{} );
    }
    const Frontend::ExprId CallId = Ast.Add( Frontend::ExprNode{
        Frontend::Call{ .Loc = {}, .Callee = MemberId, .Args = Args, .ArgNames = ArgNames, .BlockArg = {} } } );
    InferExpr( Context, CallId );
    return CallId;
}

// A synthesized `sizeof T` for a capture's own already-resolved SemaTypeId —
// never written as source syntax, so `Expr.Type` is deliberately left
// invalid. Calling InferExpr on it would route it through
// ExprInferencer's own SizeOf arm, which calls ResolveTypeExpr on that
// invalid TypeId and unconditionally overwrites the site type with the
// result (SemaTypeId{}), clobbering the answer we already know — so this
// hand-stamps exactly what that arm would have computed for an
// already-resolved operand instead of calling it (rules/backend-machine-only.md:
// the actual byte count is still left for LayoutEngine/EmitSizeOf to resolve,
// only the *type being measured* is fixed here).
Frontend::ExprId SizeOfType ( TypeCheckerContext &Context, Core::SourceRange Loc, Sema::SemaTypeId MeasuredType )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;
    const Frontend::ExprId Id = Ast.Add( Frontend::ExprNode{ Frontend::SizeOf{ .Loc = Loc, .Type = {} } } );
    const auto Base           = Context.Ctx.Types.LookupNodeKind( "IntLiteral" );
    Context.Ctx.Values.SetExprType( Id, Base ? Context.MakeType( *Base, {} ) : Sema::SemaTypeId{} );
    Context.UnconstrainedLiterals.insert( Id.Value );
    Context.Ctx.Values.SetSiteType( Sema::BindingSite{ Id }, MeasuredType );
    return Id;
}

// `((Size + 7) / 8) * 8` — round a byte size up to 8, the pointer/aggregate
// natural alignment every capture in this codebase needs (every Volt
// aggregate here is pointer/int-composed, so no per-type alignment query
// exists or is needed).
Frontend::ExprId RoundUpToEight ( TypeCheckerContext &Context, Core::SourceRange Loc, Frontend::ExprId SizeExpr )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;
    const Frontend::ExprId SevenId =
        Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( "7" ) } } );
    const Frontend::ExprId EightDivId =
        Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( "8" ) } } );
    const Frontend::ExprId EightMulId =
        Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( "8" ) } } );

    const Frontend::ExprId SumId = Ast.Add(
        Frontend::ExprNode{ Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Plus, .Lhs = SizeExpr, .Rhs = SevenId } } );
    InferExpr( Context, SumId );
    const Frontend::ExprId DivId = Ast.Add(
        Frontend::ExprNode{ Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Slash, .Lhs = SumId, .Rhs = EightDivId } } );
    InferExpr( Context, DivId );
    const Frontend::ExprId MulId = Ast.Add(
        Frontend::ExprNode{ Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Star, .Lhs = DivId, .Rhs = EightMulId } } );
    InferExpr( Context, MulId );
    return MulId;
}

// Cumulative offsets + rounded sizes for `Frame.Fields`, in order —
// `Offsets[i]` is the byte offset of field i within the env buffer,
// `Offsets.back() + <field i's own rounded size>` (folded into TotalSize) is
// the buffer's total size. Built once per closure and reused at every use
// site (capture reads inside the lifted body, the pack/unpack loops around
// the call) — all pure arithmetic over SizeOf/IntLiteral nodes, safe to
// reference the same ExprId from multiple parents.
struct ClosureFrameSizing
{
    std::vector<Frontend::ExprId> Offsets;
    Frontend::ExprId TotalSize;
};

ClosureFrameSizing SizeClosureFrame ( TypeCheckerContext &Context, Core::SourceRange Loc, const Sema::ClosureEnvFrame &Frame )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    ClosureFrameSizing Sizing;
    Frontend::ExprId RunningOffset =
        Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( "0" ) } } );
    InferExpr( Context, RunningOffset );

    Frontend::ExprId LastFieldSize;
    for ( const Sema::ClosureEnvField &Field : Frame.Fields )
    {
        Sizing.Offsets.push_back( RunningOffset );

        LastFieldSize = RoundUpToEight( Context, Loc, SizeOfType( Context, Loc, Field.Type ) );

        const Frontend::ExprId NextOffset = Ast.Add( Frontend::ExprNode{
            Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Plus, .Lhs = RunningOffset, .Rhs = LastFieldSize } } );
        InferExpr( Context, NextOffset );
        RunningOffset = NextOffset;
    }

    Sizing.TotalSize = RunningOffset;
    return Sizing;
}

// Replaces `Target`'s own content with `Replacement`'s — mutating the shared
// slot in place — unless `Context.Redirects` is set (Sema::ReinstantiateBody),
// in which case `Target` is left untouched and the substitution is recorded
// in the map instead: the same literal is walked again by every other
// instantiation of this generic body, and each needs its own answer
// (TypeCheckerContext::Redirects's own comment).
Frontend::ExprId RewriteSlot ( TypeCheckerContext &Context, Frontend::ExprId Target, Frontend::ExprId Replacement )
{
    if ( Context.Redirects != nullptr )
    {
        ( *Context.Redirects )[Target.Value] = Replacement;
        return Replacement;
    }
    Context.Ctx.Ast.Expr( Target ) = Context.Ctx.Ast.Expr( Replacement );
    return Target;
}

// Same, for a brand new node with no ExprId of its own yet: mutate mode
// stamps `Content` straight into `Target`'s slot; redirect mode `Add()`s it
// first (Target keeps its original content) and records the map entry.
Frontend::ExprId RewriteSlot ( TypeCheckerContext &Context, Frontend::ExprId Target, Frontend::ExprNode Content )
{
    if ( Context.Redirects != nullptr )
    {
        const Frontend::ExprId NewId         = Context.Ctx.Ast.Add( std::move( Content ) );
        ( *Context.Redirects )[Target.Value] = NewId;
        return NewId;
    }
    Context.Ctx.Ast.Expr( Target ) = std::move( Content );
    return Target;
}

// Rewrites every `Identifier` inside `Id`'s own subtree that resolves
// (`ScopeTable::BindingOf`) to one of `Frame.Fields`' sites, in place, into
// `*( Pointer<CapType>.from_address( EnvUse.to_address() + Offset ) )` — the
// same copy-out/write-back discipline every other arena rewrite in this file
// uses (rules/ast-rewrite.md), just scoped to a subtree by structural
// descent (`Meta::ForEachField`, reading only, never held across an `Add()`)
// rather than a flat index sweep. `FieldOffsets` is parallel to
// `Frame.Fields` — a precomputed `SizeOf`-derived offset expression per
// field (see LowerClosureLit), reused at every use site rather than rebuilt.
void RewriteCaptureUses ( TypeCheckerContext &Context,
                          Frontend::ExprId Id,
                          const Sema::ClosureEnvFrame &Frame,
                          const std::vector<Frontend::ExprId> &FieldOffsets,
                          Sema::NominalId PointerBase,
                          Core::Symbol EnvName,
                          const Sema::Binding &EnvBinding );

void RewriteCaptureUsesStmt ( TypeCheckerContext &Context,
                              Frontend::StmtId Id,
                              const Sema::ClosureEnvFrame &Frame,
                              const std::vector<Frontend::ExprId> &FieldOffsets,
                              Sema::NominalId PointerBase,
                              Core::Symbol EnvName,
                              const Sema::Binding &EnvBinding );

void RewriteOneCapture ( TypeCheckerContext &Context,
                         Frontend::ExprId Id,
                         const Sema::ClosureEnvField &Field,
                         Frontend::ExprId OffsetExpr,
                         Sema::NominalId PointerBase,
                         Core::Symbol EnvName,
                         const Sema::Binding &EnvBinding )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const Frontend::ExprId EnvUseId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = EnvName } } );
    Context.Ctx.Scopes.BindUse( EnvUseId, EnvBinding, true );
    Context.Ctx.Values.SetExprType( EnvUseId, BytePointerType( Context ) );

    const Frontend::ExprId AddrId = CallMember( Context, EnvUseId, "to_address", {} );

    const Frontend::ExprId SumId = Ast.Add(
        Frontend::ExprNode{ Frontend::Binary{ .Loc = {}, .Op = Frontend::TokenKind::Plus, .Lhs = AddrId, .Rhs = OffsetExpr } } );
    InferExpr( Context, SumId );

    Frontend::ExprList FromAddrArgs;
    FromAddrArgs.PushBack( SumId );
    const Frontend::ExprId SlotPtrId  = NakedTypeExpr( Context, PointerBase, Context.MakeType( PointerBase, { Field.Type } ) );
    const Frontend::ExprId FromAddrId = CallMember( Context, SlotPtrId, "from_address", FromAddrArgs );
    const Frontend::ExprId DerefId    = Ast.Add( Frontend::ExprNode{ Frontend::Deref{ .Loc = {}, .Operand = FromAddrId } } );
    InferExpr( Context, DerefId );

    RewriteSlot( Context, Id, DerefId );
}

void RewriteCaptureUses ( TypeCheckerContext &Context,
                          Frontend::ExprId Id,
                          const Sema::ClosureEnvFrame &Frame,
                          const std::vector<Frontend::ExprId> &FieldOffsets,
                          Sema::NominalId PointerBase,
                          Core::Symbol EnvName,
                          const Sema::Binding &EnvBinding )
{
    if ( not Id.IsValid() )
    {
        return;
    }
    if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
    {
        return;
    }

    if ( std::holds_alternative<Frontend::Identifier>( Context.Ctx.Ast.Expr( Id ) ) )
    {
        const Sema::Binding *Bound = Context.Ctx.Scopes.BindingOf( Id );
        if ( Bound == nullptr )
        {
            return;
        }
        for ( std::size_t Index = 0; Index < Frame.Fields.Size(); ++Index )
        {
            if ( Frame.Fields[Index].Site == Bound->Site )
            {
                RewriteOneCapture( Context, Id, Frame.Fields[Index], FieldOffsets[Index], PointerBase, EnvName, EnvBinding );
                return;
            }
        }
        return;
    }

    // Every child Id is copied out before any recursion — a rewrite deeper
    // in the tree may Add() and reallocate the arena backing the reference
    // ForEachField would otherwise hand back (rules/ast-rewrite.md).
    std::vector<Frontend::ExprId> ChildExprs;
    std::vector<Frontend::StmtId> ChildStmts;
    std::visit(
        [&] ( auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Meta::ForEachField( Node,
                                    [&] ( const char *, auto &Field )
                                    {
                                        using F = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                        {
                                            ChildExprs.push_back( Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                ChildExprs.push_back( Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            ChildStmts.push_back( Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                ChildStmts.push_back( Child );
                                            }
                                        }
                                    } );
            }
        },
        Context.Ctx.Ast.Expr( Id ) );

    for ( const Frontend::ExprId Child : ChildExprs )
    {
        RewriteCaptureUses( Context, Child, Frame, FieldOffsets, PointerBase, EnvName, EnvBinding );
    }
    for ( const Frontend::StmtId Child : ChildStmts )
    {
        RewriteCaptureUsesStmt( Context, Child, Frame, FieldOffsets, PointerBase, EnvName, EnvBinding );
    }
}

void RewriteCaptureUsesStmt ( TypeCheckerContext &Context,
                              Frontend::StmtId Id,
                              const Sema::ClosureEnvFrame &Frame,
                              const std::vector<Frontend::ExprId> &FieldOffsets,
                              Sema::NominalId PointerBase,
                              Core::Symbol EnvName,
                              const Sema::Binding &EnvBinding )
{
    if ( not Id.IsValid() )
    {
        return;
    }

    std::vector<Frontend::ExprId> ChildExprs;
    std::vector<Frontend::StmtId> ChildStmts;
    std::visit(
        [&] ( auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Meta::ForEachField( Node,
                                    [&] ( const char *, auto &Field )
                                    {
                                        using F = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                        {
                                            ChildExprs.push_back( Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                ChildExprs.push_back( Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            ChildStmts.push_back( Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                ChildStmts.push_back( Child );
                                            }
                                        }
                                    } );
            }
        },
        Context.Ctx.Ast.Stmt( Id ) );

    for ( const Frontend::ExprId Child : ChildExprs )
    {
        RewriteCaptureUses( Context, Child, Frame, FieldOffsets, PointerBase, EnvName, EnvBinding );
    }
    for ( const Frontend::StmtId Child : ChildStmts )
    {
        RewriteCaptureUsesStmt( Context, Child, Frame, FieldOffsets, PointerBase, EnvName, EnvBinding );
    }
}

} // namespace

void Volt::Sema::TypeCheckerPass::LowerClosureLit ( TypeCheckerContext &Context, Frontend::ExprId Id )
{
    const SemaTypeId LiteralType = Context.Ctx.Values.ExprType( Id );
    // Under a generic definition (`Enumerable<T>`'s own body, T unresolved),
    // Id was marked deferred the moment it was typed (UnitTypes::MarkDeferred,
    // ExprInferencer's InferExpr) even though ComputeExpr still interned some
    // SemaType for it — `Has()` alone cannot tell the two apart, only
    // IsDeferred can (core-ast.md's "Generic definition bodies"). Left
    // un-lowered here, Sema::ReinstantiateBody lowers it once per concrete
    // instantiation instead, each with its own answer for T.
    if ( not LiteralType.IsValid() or Context.Ctx.Values.IsDeferred( Id ) )
    {
        return;
    }

    const ScopeId ClosureScope  = Context.Ctx.Scopes.ScopeOfExpr( Id );
    const ClosureEnvFrame Frame = ClosureScope.IsValid()
                                      ? SynthesizeClosureFrame( Context.Ctx.Scopes, Context.Ctx.Values, ClosureScope )
                                      : ClosureEnvFrame{};

    // Positional: SynthesizeClosureFrame builds Frame.Fields by iterating
    // ScopeTable::CapturesOf in order, so index i of one is index i of the
    // other — Frame.Fields carries Name/Site/Type, Captures carries the
    // DeclaringScope a fresh read of the captured variable needs.
    const Core::SmallVec<Capture, 4> *Captures = ClosureScope.IsValid() ? Context.Ctx.Scopes.CapturesOf( ClosureScope ) : nullptr;

    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Copied out before any Add(): the source variant is read once, by
    // value, before this rewrite starts appending to the same arenas
    // (rules/ast-rewrite.md).
    const bool bIsLambda = std::holds_alternative<Frontend::Lambda>( Ast.Expr( Id ) );

    Core::SourceRange Loc;
    Frontend::ParamList Params;
    Frontend::StmtList Body;
    if ( bIsLambda )
    {
        const Frontend::Lambda Node = std::get<Frontend::Lambda>( Ast.Expr( Id ) );
        Loc                         = Node.Loc;
        Params                      = Node.Params;
        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Node.Loc, .Expr = Node.Body } } ) );
    }
    else
    {
        const Frontend::Block Node = std::get<Frontend::Block>( Ast.Expr( Id ) );
        Loc                        = Node.Loc;
        Params                     = Node.Params;
        Body                       = Node.Body;
    }

    const SemaTypeId BytePtr    = BytePointerType( Context );
    const NominalId PointerBase = Context.Ctx.Values.Has( BytePtr ) ? Context.Ctx.Values.Get( BytePtr ).Base : NominalId{};

    // Every synthesized closure function ends its parameter list with the
    // env — present even when nothing is captured, so ClosureEmitter's
    // EmitIndirectCall (which always appends the trailing env argument to
    // its call-site signature) needs no second calling convention.
    Frontend::Param EnvParam;
    EnvParam.Loc                       = Loc;
    EnvParam.Name                      = Ast.MakeUniqueSymbol( "__env" );
    const Frontend::ParamId EnvParamId = Ast.Add( EnvParam );
    Context.Ctx.Values.SetSiteType( BindingSite{ EnvParamId }, BytePtr );
    Params.PushBack( EnvParamId );

    const bool bHasCaptures = not Frame.Fields.IsEmpty();

    // Byte offset (within the env buffer) and rounded byte size per field,
    // built from `sizeof <field's own type>` rather than a fixed constant —
    // a capture's real width (an aggregate like Array<String> is 24 bytes,
    // wider than the pointer/int fields this used to assume) is only known
    // once the field's own SemaTypeId is concrete, exactly like any other
    // generic-body value (rules/backend-machine-only.md). Computed once,
    // reused at every use site below.
    const ClosureFrameSizing Sizing = bHasCaptures ? SizeClosureFrame( Context, Loc, Frame ) : ClosureFrameSizing{};

    if ( bHasCaptures )
    {
        // Declared into a real scope so the Binding lives in ScopeTable's
        // own heap-stable node storage — BindUse stores a raw `const
        // Binding *`, so a locally-scoped temporary would dangle by the
        // time codegen reads it back, long after this pass has returned
        // (rules/ast-value.md's arena-stability concern, applied to
        // ScopeTable rather than the AST arenas).
        Context.Ctx.Scopes.Declare( ClosureScope, EnvParam.Name, BindingSite{ EnvParamId } );
        const Binding *EnvBound = Context.Ctx.Scopes.Resolve( ClosureScope, EnvParam.Name );
        if ( EnvBound != nullptr )
        {
            for ( const Frontend::StmtId StmtId : Body )
            {
                RewriteCaptureUsesStmt( Context, StmtId, Frame, Sizing.Offsets, PointerBase, EnvParam.Name, *EnvBound );
            }
        }
    }

    Frontend::Method Synth;
    Synth.Loc    = Loc;
    Synth.Name   = Ast.MakeUniqueSymbol( "__closure" );
    Synth.Params = Params;
    Synth.Body   = Body;

    const Frontend::DeclId NewDecl = Ast.Add( Frontend::DeclNode{ std::move( Synth ) } );
    Ast.TopDecls.push_back( NewDecl );

    const SemaType &Closure = Context.Ctx.Values.Get( LiteralType );
    const SemaTypeId Result = Closure.Args.IsEmpty() ? SemaTypeId{} : Closure.Args[0];
    Core::SmallVec<SemaTypeId, 4> ParamTypes;
    for ( std::size_t Index = 1; Index < Closure.Args.Size(); ++Index )
    {
        ParamTypes.PushBack( Closure.Args[Index] );
    }
    ParamTypes.PushBack( BytePtr );
    Context.Ctx.Synth.Add( SynthesizedFunction{ .Decl = NewDecl, .Result = Result, .Params = std::move( ParamTypes ) } );

    const std::string_view NameText = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Closure.Base ).Name );
    const Frontend::ExprId ObjectId =
        Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Ast.Strings().Intern( NameText ) } } );
    Context.Ctx.Values.SetExprType( ObjectId, LiteralType );
    Context.NakedTypeExprs.insert( ObjectId.Value );

    const Frontend::ExprId NewMemberId = Ast.Add(
        Frontend::ExprNode{ Frontend::Member{ .Loc = Loc, .Object = ObjectId, .Name = Ast.Strings().Intern( "new" ) } } );
    const Frontend::ExprId FuncAddrId = Ast.Add( Frontend::ExprNode{ Frontend::FuncAddr{ .Loc = Loc, .Target = NewDecl } } );

    if ( not bHasCaptures )
    {
        const Frontend::ExprId EnvId = Ast.Add( Frontend::ExprNode{ Frontend::NilLiteral{ .Loc = Loc } } );

        Frontend::ExprList Args;
        Args.PushBack( FuncAddrId );
        Args.PushBack( EnvId );
        Frontend::SymbolList ArgNames;
        ArgNames.PushBack( Core::Symbol{} );
        ArgNames.PushBack( Core::Symbol{} );

        const Frontend::ExprId CtorCallId = Ast.Add( Frontend::ExprNode{
            Frontend::Call{ .Loc = Loc, .Callee = NewMemberId, .Args = Args, .ArgNames = ArgNames, .BlockArg = {} } } );

        InferExpr( Context, CtorCallId );

        if ( const auto Entry = Context.CalleeResolution.find( CtorCallId.Value ); Entry != Context.CalleeResolution.end() )
        {
            Context.CalleeResolution[Id.Value] = Entry->second;
        }
        if ( const SemaTypeId CallTypeRes = Context.Ctx.Values.ExprType( CtorCallId ); CallTypeRes.IsValid() )
        {
            Context.Ctx.Values.SetExprType( Id, CallTypeRes );
        }

        RewriteSlot( Context, Id, CtorCallId );
        return;
    }

    const ScopeId CurrentScope = ClosureScope.IsValid() ? ClosureScope : ScopeId{ 0 };

    // A heap-allocated environment: capture storage must outlive the
    // enclosing frame the moment a closure escapes it (the conservative
    // default `ClosureEnvFrame::bEscapes` already assumes), and Pointer<T>
    // already exposes malloc/to_address/from_address as ordinary Volt
    // members — every backend gets this for free from its existing
    // Deref/Call handling, no backend-side closure knowledge required
    // (rules/backend-machine-only.md).
    Frontend::ExprList MallocArgs;
    MallocArgs.PushBack( Sizing.TotalSize );
    const Frontend::ExprId MallocObjId  = NakedTypeExpr( Context, PointerBase, BytePtr );
    const Frontend::ExprId MallocCallId = CallMember( Context, MallocObjId, "malloc", MallocArgs );

    const Frontend::Symbol TmpName   = Ast.MakeUniqueSymbol( "__env" );
    const Frontend::ExprId TmpTarget = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = TmpName } } );

    // Declare/BindUse *before* inferring the Assign — WriteLocal (invoked
    // from the Assign arm below) resolves this site through
    // ScopeTable::BindingOf(Use), so the binding must already exist by the
    // time InferExpr walks it, exactly as LiteralLowering's own `tmp =
    // T.new()` does it. Doing this after InferExpr leaves SiteOf with
    // nothing to find, so Ctx.Values.SetSiteType never runs and codegen
    // later reports "local has no resolved layout".
    Context.Ctx.Scopes.Declare( CurrentScope, TmpName, BindingSite{ TmpTarget } );
    const Binding *TmpBound = Context.Ctx.Scopes.Resolve( CurrentScope, TmpName );
    if ( TmpBound != nullptr )
    {
        Context.Ctx.Scopes.BindUse( TmpTarget, *TmpBound, false );
    }

    Frontend::StmtList EnsureBody;
    for ( std::size_t Index = 0; Index < Frame.Fields.Size(); ++Index )
    {
        const ClosureEnvField &Field = Frame.Fields[Index];

        const Frontend::ExprId TmpUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = TmpName } } );
        if ( TmpBound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( TmpUse, *TmpBound, true );
        }
        Context.Ctx.Values.SetExprType( TmpUse, BytePtr );
        const Frontend::ExprId AddrId = CallMember( Context, TmpUse, "to_address", {} );

        const Frontend::ExprId SumId = Ast.Add( Frontend::ExprNode{
            Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Plus, .Lhs = AddrId, .Rhs = Sizing.Offsets[Index] } } );
        InferExpr( Context, SumId );

        Frontend::ExprList FromAddrArgs;
        FromAddrArgs.PushBack( SumId );
        const Frontend::ExprId SlotPtrId = NakedTypeExpr( Context, PointerBase, Context.MakeType( PointerBase, { Field.Type } ) );
        const Frontend::ExprId FromAddrId = CallMember( Context, SlotPtrId, "from_address", FromAddrArgs );
        const Frontend::ExprId DerefId    = Ast.Add( Frontend::ExprNode{ Frontend::Deref{ .Loc = Loc, .Operand = FromAddrId } } );
        InferExpr( Context, DerefId );

        const Binding *CapBound        = Captures != nullptr and Index < Captures->Size()
                                             ? Context.Ctx.Scopes.Resolve( ( *Captures )[Index].DeclaringScope, Field.Name )
                                             : nullptr;
        const Frontend::ExprId WriteId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Field.Name } } );
        if ( CapBound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( WriteId, *CapBound, false );
        }
        Context.Ctx.Values.SetExprType( WriteId, Field.Type );

        const Frontend::ExprId WriteBackId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = WriteId, .Value = DerefId } } );
        InferExpr( Context, WriteBackId );

        EnsureBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = WriteBackId } } ) );
    }

    const Frontend::ExprId AssignId =
        Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = TmpTarget, .Value = MallocCallId } } );
    InferExpr( Context, AssignId );

    Frontend::StmtList OuterBody;
    OuterBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = AssignId } } ) );

    for ( std::size_t Index = 0; Index < Frame.Fields.Size(); ++Index )
    {
        const ClosureEnvField &Field = Frame.Fields[Index];

        const Frontend::ExprId TmpUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = TmpName } } );
        if ( TmpBound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( TmpUse, *TmpBound, true );
        }
        Context.Ctx.Values.SetExprType( TmpUse, BytePtr );
        const Frontend::ExprId AddrId = CallMember( Context, TmpUse, "to_address", {} );

        const Frontend::ExprId SumId = Ast.Add( Frontend::ExprNode{
            Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Plus, .Lhs = AddrId, .Rhs = Sizing.Offsets[Index] } } );
        InferExpr( Context, SumId );

        Frontend::ExprList FromAddrArgs;
        FromAddrArgs.PushBack( SumId );
        const Frontend::ExprId SlotPtrId = NakedTypeExpr( Context, PointerBase, Context.MakeType( PointerBase, { Field.Type } ) );
        const Frontend::ExprId FromAddrId = CallMember( Context, SlotPtrId, "from_address", FromAddrArgs );
        const Frontend::ExprId DerefId    = Ast.Add( Frontend::ExprNode{ Frontend::Deref{ .Loc = Loc, .Operand = FromAddrId } } );
        InferExpr( Context, DerefId );

        const Binding *CapBound       = Captures != nullptr and Index < Captures->Size()
                                            ? Context.Ctx.Scopes.Resolve( ( *Captures )[Index].DeclaringScope, Field.Name )
                                            : nullptr;
        const Frontend::ExprId ReadId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Field.Name } } );
        if ( CapBound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( ReadId, *CapBound, true );
        }
        Context.Ctx.Values.SetExprType( ReadId, Field.Type );

        const Frontend::ExprId StoreId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = DerefId, .Value = ReadId } } );
        InferExpr( Context, StoreId );

        OuterBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = StoreId } } ) );
    }

    const Frontend::ExprId FinalEnvUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = TmpName } } );
    if ( TmpBound != nullptr )
    {
        Context.Ctx.Scopes.BindUse( FinalEnvUse, *TmpBound, true );
    }
    Context.Ctx.Values.SetExprType( FinalEnvUse, BytePtr );

    Frontend::ExprList CtorArgs;
    CtorArgs.PushBack( FuncAddrId );
    CtorArgs.PushBack( FinalEnvUse );
    Frontend::SymbolList CtorArgNames;
    CtorArgNames.PushBack( Core::Symbol{} );
    CtorArgNames.PushBack( Core::Symbol{} );
    const Frontend::ExprId CtorCallId = Ast.Add( Frontend::ExprNode{
        Frontend::Call{ .Loc = Loc, .Callee = NewMemberId, .Args = CtorArgs, .ArgNames = CtorArgNames, .BlockArg = {} } } );
    InferExpr( Context, CtorCallId );

    Frontend::ExprId ParentCallId;
    for ( std::size_t Index = 0; Index < Context.Ctx.Ast.ExprCount(); ++Index )
    {
        const Frontend::ExprId CandidateId{ static_cast<Frontend::ExprId::ValueType>( Index ) };
        if ( CandidateId == Id )
        {
            continue;
        }
        if ( const auto *CallNode = std::get_if<Frontend::Call>( &Ast.Expr( CandidateId ) ) )
        {
            if ( CallNode->BlockArg == Id )
            {
                ParentCallId = CandidateId;
                break;
            }
        }
    }

    if ( ParentCallId.IsValid() )
    {
        RewriteSlot( Context, Id, CtorCallId );

        const auto &OrigCall             = std::get<Frontend::Call>( Ast.Expr( ParentCallId ) );
        const Frontend::ExprId NewCallId = Ast.Add( Frontend::ExprNode{ Frontend::Call{ .Loc      = OrigCall.Loc,
                                                                                        .Callee   = OrigCall.Callee,
                                                                                        .Args     = OrigCall.Args,
                                                                                        .ArgNames = OrigCall.ArgNames,
                                                                                        .BlockArg = Id } } );
        InferExpr( Context, NewCallId );

        OuterBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = NewCallId } } ) );

        const SemaTypeId ParentCallType = Context.Ctx.Values.ExprType( ParentCallId );

        RewriteSlot(
            Context, ParentCallId,
            Frontend::ExprNode{ Frontend::BeginExpr{
                .Loc = Loc, .Body = std::move( OuterBody ), .RescueClauses = {}, .EnsureBody = std::move( EnsureBody ) } } );

        if ( ParentCallType.IsValid() )
        {
            Context.Ctx.Values.SetExprType( ParentCallId, ParentCallType );
        }
    }
    else
    {
        OuterBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CtorCallId } } ) );

        if ( const SemaTypeId CallTypeRes = Context.Ctx.Values.ExprType( CtorCallId ); CallTypeRes.IsValid() )
        {
            Context.Ctx.Values.SetExprType( Id, CallTypeRes );
        }

        RewriteSlot(
            Context, Id,
            Frontend::ExprNode{ Frontend::BeginExpr{
                .Loc = Loc, .Body = std::move( OuterBody ), .RescueClauses = {}, .EnsureBody = std::move( EnsureBody ) } } );
    }
}

void Volt::Sema::TypeCheckerPass::LowerClosureLits ( TypeCheckerContext &Context )
{
    const std::size_t OriginalCount = Context.Ctx.Ast.ExprCount();
    for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
    {
        const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };

        if ( Id.Value < Context.Metadata.size() and Context.Metadata[Id.Value] )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::Lambda>( Context.Ctx.Ast.Expr( Id ) ) and
             not std::holds_alternative<Frontend::Block>( Context.Ctx.Ast.Expr( Id ) ) )
        {
            continue;
        }

        LowerClosureLit( Context, Id );
    }
}
