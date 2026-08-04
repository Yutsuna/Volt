#include "ClosureLifting.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Layout/ClosureFrame.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <string>
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

// Rewrites every `Identifier` inside `Id`'s own subtree that resolves
// (`ScopeTable::BindingOf`) to one of `Frame.Fields`' sites, in place, into
// `*( Pointer<CapType>.from_address( EnvUse.to_address() + Offset ) )` — the
// same copy-out/write-back discipline every other arena rewrite in this file
// uses (rules/ast-rewrite.md), just scoped to a subtree by structural
// descent (`Meta::ForEachField`, reading only, never held across an `Add()`)
// rather than a flat index sweep.
void RewriteCaptureUses ( TypeCheckerContext &Context,
                          Frontend::ExprId Id,
                          const Sema::ClosureEnvFrame &Frame,
                          Sema::NominalId PointerBase,
                          Frontend::ParamId EnvParamId,
                          const Sema::Binding &EnvBinding );

void RewriteCaptureUsesStmt ( TypeCheckerContext &Context,
                              Frontend::StmtId Id,
                              const Sema::ClosureEnvFrame &Frame,
                              Sema::NominalId PointerBase,
                              Frontend::ParamId EnvParamId,
                              const Sema::Binding &EnvBinding );

void RewriteOneCapture ( TypeCheckerContext &Context,
                         Frontend::ExprId Id,
                         const Sema::ClosureEnvField &Field,
                         Sema::NominalId PointerBase,
                         Frontend::ParamId EnvParamId,
                         const Sema::Binding &EnvBinding )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    const Frontend::ExprId EnvUseId =
        Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = {}, .Name = Ast.GetParam( EnvParamId ).Name } } );
    Context.Ctx.Scopes.BindUse( EnvUseId, EnvBinding, true );
    Context.Ctx.Values.SetExprType( EnvUseId, BytePointerType( Context ) );

    const Frontend::ExprId AddrId = CallMember( Context, EnvUseId, "to_address", {} );

    const std::string OffsetText = std::to_string( Field.Offset );
    const Frontend::ExprId OffsetId =
        Ast.Add( Frontend::ExprNode{ Frontend::IntLiteral{ .Loc = {}, .Raw = Ast.Strings().Intern( OffsetText ) } } );

    const Frontend::ExprId SumId = Ast.Add(
        Frontend::ExprNode{ Frontend::Binary{ .Loc = {}, .Op = Frontend::TokenKind::Plus, .Lhs = AddrId, .Rhs = OffsetId } } );
    InferExpr( Context, SumId );

    Frontend::ExprList FromAddrArgs;
    FromAddrArgs.PushBack( SumId );
    const Frontend::ExprId SlotPtrId  = NakedTypeExpr( Context, PointerBase, Context.MakeType( PointerBase, { Field.Type } ) );
    const Frontend::ExprId FromAddrId = CallMember( Context, SlotPtrId, "from_address", FromAddrArgs );
    const Frontend::ExprId DerefId    = Ast.Add( Frontend::ExprNode{ Frontend::Deref{ .Loc = {}, .Operand = FromAddrId } } );
    InferExpr( Context, DerefId );

    Ast.Expr( Id ) = Ast.Expr( DerefId );
}

void RewriteCaptureUses ( TypeCheckerContext &Context,
                          Frontend::ExprId Id,
                          const Sema::ClosureEnvFrame &Frame,
                          Sema::NominalId PointerBase,
                          Frontend::ParamId EnvParamId,
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
        for ( const Sema::ClosureEnvField &Field : Frame.Fields )
        {
            if ( Field.Site == Bound->Site )
            {
                RewriteOneCapture( Context, Id, Field, PointerBase, EnvParamId, EnvBinding );
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
        RewriteCaptureUses( Context, Child, Frame, PointerBase, EnvParamId, EnvBinding );
    }
    for ( const Frontend::StmtId Child : ChildStmts )
    {
        RewriteCaptureUsesStmt( Context, Child, Frame, PointerBase, EnvParamId, EnvBinding );
    }
}

void RewriteCaptureUsesStmt ( TypeCheckerContext &Context,
                              Frontend::StmtId Id,
                              const Sema::ClosureEnvFrame &Frame,
                              Sema::NominalId PointerBase,
                              Frontend::ParamId EnvParamId,
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
        RewriteCaptureUses( Context, Child, Frame, PointerBase, EnvParamId, EnvBinding );
    }
    for ( const Frontend::StmtId Child : ChildStmts )
    {
        RewriteCaptureUsesStmt( Context, Child, Frame, PointerBase, EnvParamId, EnvBinding );
    }
}

bool ExprHasBreakOrNext ( const TypeCheckerContext &Context, Frontend::ExprId Id );

// `break`/`next` inside a lifted closure body would compile through the
// ordinary top-level function emitter rather than ClosureEmitter's
// EmitClosureBody, which is where the non-local-exit-as-unwind-sentinel
// translation lives (`EmitUnwindCheck`, called after every indirect call,
// consumes it) — so a lifted body's own `break`/`next` statements reach
// codegen as an ordinary loop-relative jump with no loop to jump out of.
// Reconciling that is Phase 5's job (`.agents/PLAN_CLOSURE_LOWERING.md`);
// until then, a closure whose own body contains one is left as an
// unlowered Lambda/Block, exactly as every capturing closure was before
// this pass existed. A nested Lambda/Block's own `break`/`next` belongs to
// that inner closure's scope, not this one, so the scan does not descend
// into one.
bool StmtHasBreakOrNext ( const TypeCheckerContext &Context, Frontend::StmtId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    if ( std::holds_alternative<Frontend::Break>( Context.Ctx.Ast.Stmt( Id ) ) or
         std::holds_alternative<Frontend::Next>( Context.Ctx.Ast.Stmt( Id ) ) )
    {
        return true;
    }

    bool bFound = false;
    std::visit(
        [&] ( const auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Meta::ForEachField( Node,
                                    [&] ( const char *, const auto &Field )
                                    {
                                        using F = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                        {
                                            bFound = bFound or ExprHasBreakOrNext( Context, Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                bFound = bFound or ExprHasBreakOrNext( Context, Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            bFound = bFound or StmtHasBreakOrNext( Context, Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                bFound = bFound or StmtHasBreakOrNext( Context, Child );
                                            }
                                        }
                                    } );
            }
        },
        Context.Ctx.Ast.Stmt( Id ) );
    return bFound;
}

bool ExprHasBreakOrNext ( const TypeCheckerContext &Context, Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    // A nested closure literal owns its own `break`/`next` target — do not
    // descend into it.
    if ( std::holds_alternative<Frontend::Lambda>( Context.Ctx.Ast.Expr( Id ) ) or
         std::holds_alternative<Frontend::Block>( Context.Ctx.Ast.Expr( Id ) ) )
    {
        return false;
    }

    bool bFound = false;
    std::visit(
        [&] ( const auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( not std::is_same_v<T, std::monostate> )
            {
                Meta::ForEachField( Node,
                                    [&] ( const char *, const auto &Field )
                                    {
                                        using F = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                        {
                                            bFound = bFound or ExprHasBreakOrNext( Context, Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                bFound = bFound or ExprHasBreakOrNext( Context, Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                        {
                                            bFound = bFound or StmtHasBreakOrNext( Context, Field );
                                        }
                                        else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                bFound = bFound or StmtHasBreakOrNext( Context, Child );
                                            }
                                        }
                                    } );
            }
        },
        Context.Ctx.Ast.Expr( Id ) );
    return bFound;
}

} // namespace

void Volt::Sema::TypeCheckerPass::LowerClosureLit ( TypeCheckerContext &Context, Frontend::ExprId Id )
{
    const SemaTypeId LiteralType = Context.Ctx.Values.ExprType( Id );
    if ( not LiteralType.IsValid() or not Context.Ctx.Values.Has( LiteralType ) )
    {
        return;
    }

    const ScopeId ClosureScope  = Context.Ctx.Scopes.ScopeOfExpr( Id );
    const ClosureEnvFrame Frame = ClosureScope.IsValid()
                                      ? SynthesizeClosureFrame( Context.Ctx.Scopes, Context.Ctx.Values, ClosureScope )
                                      : ClosureEnvFrame{};

    // Positional: SynthesizeClosureFrame builds Frame.Fields by iterating
    // ScopeTable::CapturesOf in order, so index i of one is index i of the
    // other — Frame.Fields carries Offset/Type, Captures carries the
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

    for ( const Frontend::StmtId StmtId : Body )
    {
        if ( StmtHasBreakOrNext( Context, StmtId ) )
        {
            return;
        }
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
                RewriteCaptureUsesStmt( Context, StmtId, Frame, PointerBase, EnvParamId, *EnvBound );
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

        Ast.Expr( Id ) = Ast.Expr( CtorCallId );
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
    MallocArgs.PushBack( Ast.Add( Frontend::ExprNode{
        Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( std::to_string( Frame.TotalSize ) ) } } ) );
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

        const Frontend::ExprId OffsetId = Ast.Add( Frontend::ExprNode{
            Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( std::to_string( Field.Offset ) ) } } );
        const Frontend::ExprId SumId    = Ast.Add( Frontend::ExprNode{
            Frontend::Binary{ .Loc = Loc, .Op = Frontend::TokenKind::Plus, .Lhs = AddrId, .Rhs = OffsetId } } );
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

    OuterBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CtorCallId } } ) );

    if ( const SemaTypeId CallTypeRes = Context.Ctx.Values.ExprType( CtorCallId ); CallTypeRes.IsValid() )
    {
        Context.Ctx.Values.SetExprType( Id, CallTypeRes );
    }

    Ast.Expr( Id ) = Frontend::ExprNode{
        Frontend::BeginExpr{ .Loc = Loc, .Body = std::move( OuterBody ), .RescueClauses = {}, .EnsureBody = {} } };
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
