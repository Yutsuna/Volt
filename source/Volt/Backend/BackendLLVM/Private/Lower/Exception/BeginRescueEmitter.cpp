// BeginRescueEmitter.cpp — `begin / rescue / ensure`, the handler side.
//
// The clause ladder tests the raised object's own NominalId against each
// clause's *resolved* filter (Values->SiteType(BindingSite{ClauseId}), which
// ExprInferencer records for every clause, bound variable or not) by walking the
// ancestry table at runtime — the same reflexive test Sema's IsSubclassOf
// performs, done here because only the dynamic side is unknown at compile time.
//
// Three subtleties, all load-bearing:
//   - a raise from inside a *clause body* must not re-enter this begin's own
//     ladder, so the rescue target changes for exactly that span;
//   - a matched clause clears the in-flight state *before* running the handler,
//     so a call inside it is checked against whatever it raises, not against
//     what it is handling;
//   - `ensure` still has to run for an unhandled exception, and what is still
//     pending afterwards must be re-propagated — including a `break`, which
//     sails through every clause untouched (the ancestor test is false while the
//     tag is still InvalidValue) and would otherwise be silently absorbed.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <string>
#include <variant>

void Volt::Backend::Llvm::ExceptionLowering::EmitBeginTail (
    BodyEmitter &Emitter, const Frontend::StmtList &Body, llvm::AllocaInst *Slot, llvm::Type *Shape, Sema::LayoutId Layout )
{
    const Frontend::AstContext &Ast = *Emitter.Frame().Unit->Ast;
    for ( std::size_t Index = 0; Index < Body.Size() and not Emitter.Terminated() and not Emitter.Failed(); ++Index )
    {
        const bool bLast = Index + 1 == Body.Size();
        if ( bLast and Slot != nullptr )
        {
            if ( const auto *Tail = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Index] ) ); Tail != nullptr )
            {
                Emitter.StoreTailValue( Emitter.EmitExpr( Tail->Expr ), Slot, Shape, Layout );
                continue;
            }
        }
        Emitter.EmitStmt( Body[Index], false );
    }
}

llvm::Value *
Volt::Backend::Llvm::ExceptionLowering::EmitBegin ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::BeginExpr &Node )
{
    FunctionFrame &Frame            = Emitter.Frame();
    const Frontend::AstContext &Ast = *Frame.Unit->Ast;
    const Sema::UnitTypes &Values   = *Frame.Values;
    llvm::LLVMContext &Context      = Services->Ctx->Context();
    llvm::IRBuilder<> &Builder      = Services->Ctx->Builder();
    llvm::Function *Fn              = Frame.Fn;

    // A `begin` in value position needs a slot for the same reason `case` does:
    // its body and every clause are statement lists, so their results converge
    // through storage rather than a phi over a block count fixed only once the
    // clause ladder is built. An aggregate result gets a slot too —
    // `StoreTailValue` converges it by `EmitStore`'s memcpy, not a plain
    // `CreateStore` of an SSA struct value.
    llvm::Type *Shape           = Emitter.TypeOfExpr( Id );
    const Sema::LayoutId Layout = Emitter.LayoutOfExpr( Id );
    llvm::AllocaInst *Slot      = nullptr;
    if ( Shape != nullptr )
    {
        Slot = Emitter.MakeTemp( Shape, "begin.result" );
    }

    llvm::BasicBlock *Dispatch = llvm::BasicBlock::Create( Context, "rescue.dispatch", Fn );
    llvm::BasicBlock *Ensure   = llvm::BasicBlock::Create( Context, "begin.ensure", Fn );
    llvm::BasicBlock *Merge    = llvm::BasicBlock::Create( Context, "begin.end", Fn );

    // The body's own raises, and any call inside it that finds one pending, land
    // in Dispatch — this begin gets first refusal.
    Frame.Rescues.push_back( Dispatch );
    EmitBeginTail( Emitter, Node.Body, Slot, Shape, Layout );
    Frame.Rescues.pop_back();
    if ( not Emitter.Terminated() )
    {
        // Completed with nothing pending: `rescue` never runs, straight to
        // `ensure`.
        static_cast<void>( Builder.CreateBr( Ensure ) );
    }

    Builder.SetInsertPoint( Dispatch );
    if ( Emitter.Failed() )
    {
        return nullptr;
    }
    llvm::Value *Tag    = Builder.CreateLoad( llvm::Type::getInt32Ty( Context ), ExceptionTagSlot(), "exc.tag" );
    llvm::Value *ExcPtr = Builder.CreateLoad( llvm::PointerType::get( Context, 0 ), ExceptionValueSlot(), "exc.value" );

    // A raise reaching here from inside a *clause's own* body must not re-enter
    // this begin's own clause ladder — it skips straight to `ensure`, which still
    // has to run — so the target changes for exactly the span of the clause
    // bodies below.
    Frame.Rescues.push_back( Ensure );

    for ( const Frontend::StmtId ClauseId : Node.RescueClauses )
    {
        if ( not ClauseId.IsValid() )
        {
            continue;
        }
        const auto *Clause = std::get_if<Frontend::RescueClause>( &Ast.Stmt( ClauseId ) );
        if ( Clause == nullptr )
        {
            static_cast<void>( Emitter.Fail( "llvm: a `rescue` clause is not a RescueClause" ) );
            Frame.Rescues.pop_back();
            return nullptr;
        }

        // ExprInferencer records this for every clause, bound variable or not —
        // the filter Sema resolved, never re-resolved here.
        const Sema::SemaTypeId FilterType = Values.SiteType( Sema::BindingSite{ ClauseId } );
        if ( not Values.Has( FilterType ) or not Values.Get( FilterType ).Base.IsValid() )
        {
            static_cast<void>( Emitter.Fail( "llvm: `rescue` clause at statement " + std::to_string( ClauseId.Value ) +
                                             " has no resolved filter type" ) );
            Frame.Rescues.pop_back();
            return nullptr;
        }
        const Sema::NominalId Target = Values.Get( FilterType ).Base;

        llvm::Value *Matches = EmitAncestorTest( Emitter, Tag, Target );

        llvm::BasicBlock *Body = llvm::BasicBlock::Create( Context, "rescue.body", Fn );
        llvm::BasicBlock *Next = llvm::BasicBlock::Create( Context, "rescue.next", Fn );
        static_cast<void>( Builder.CreateCondBr( Matches, Body, Next ) );

        Builder.SetInsertPoint( Body );
        // Handled: clear the in-flight state before running the handler, so a
        // call inside it is checked against whatever *it* raises, never the
        // exception it is currently handling.
        static_cast<void>( Builder.CreateStore(
            llvm::ConstantInt::get( llvm::Type::getInt32Ty( Context ), Sema::NominalId::InvalidValue ), ExceptionTagSlot() ) );
        static_cast<void>(
            Builder.CreateStore( llvm::Constant::getNullValue( llvm::PointerType::get( Context, 0 ) ), ExceptionValueSlot() ) );

        if ( Clause->VarName.IsValid() )
        {
            const Sema::LayoutId FilterShape = Emitter.Types().LayoutOfValue( Values, FilterType );
            llvm::Type *VarType              = Emitter.Types().TypeOfLayout( FilterShape );
            if ( VarType == nullptr )
            {
                static_cast<void>( Emitter.Fail( "llvm: rescue variable '" +
                                                 std::string( Frame.Unit->Ast->Text( Clause->VarName ) ) +
                                                 "' has no resolved layout" ) );
                Frame.Rescues.pop_back();
                return nullptr;
            }
            llvm::Value *VarSlot =
                Emitter.SlotFor( Sema::BindingSite{ ClauseId }, VarType, Frame.Unit->Ast->Text( Clause->VarName ) );
            if ( VarSlot == nullptr )
            {
                Frame.Rescues.pop_back();
                return nullptr;
            }
            Emitter.EmitStore( VarSlot, ExcPtr, FilterShape );
        }

        EmitBeginTail( Emitter, Clause->Body, Slot, Shape, Layout );
        if ( not Emitter.Terminated() )
        {
            static_cast<void>( Builder.CreateBr( Ensure ) );
        }

        Builder.SetInsertPoint( Next );
        if ( Emitter.Failed() )
        {
            Frame.Rescues.pop_back();
            return nullptr;
        }
    }

    Frame.Rescues.pop_back();

    // Fell through every clause with nothing matching: still unhandled. The state
    // is left exactly as Dispatch found it, so the recheck after `ensure` below
    // re-propagates it.
    if ( not Emitter.Terminated() )
    {
        static_cast<void>( Builder.CreateBr( Ensure ) );
    }

    Builder.SetInsertPoint( Ensure );

    // Whatever is pending on entry here (an unhandled exception that fell
    // through every clause, or a `break` sailing through) must not leak into
    // EnsureBody's own execution: every resolved call checks the exception
    // tag / break flag *after itself* (EmitExceptionCheck / EmitUnwindCheck,
    // ExprResolvedCallEmitter.cpp) to decide whether to propagate — with
    // nothing done here, the first call inside `ensure` finds the leftover
    // pending state, mistakes it for something it just raised, and
    // propagates immediately, silently skipping every EnsureBody statement
    // after it. Save it, run under a clean slate, restore afterward — unless
    // EnsureBody raised something of its own, which is already mid-
    // propagation (Emitter.Terminated()) and takes priority over what was
    // saved.
    llvm::Value *SavedTag = Builder.CreateLoad( llvm::Type::getInt32Ty( Context ), ExceptionTagSlot(), "exc.tag.saved" );
    llvm::Value *SavedExc = Builder.CreateLoad( llvm::PointerType::get( Context, 0 ), ExceptionValueSlot(), "exc.value.saved" );
    llvm::Value *SavedBrk = Builder.CreateLoad( Builder.getInt1Ty(), BreakFlagSlot(), "brk.saved" );
    static_cast<void>( Builder.CreateStore(
        llvm::ConstantInt::get( llvm::Type::getInt32Ty( Context ), Sema::NominalId::InvalidValue ), ExceptionTagSlot() ) );
    static_cast<void>(
        Builder.CreateStore( llvm::Constant::getNullValue( llvm::PointerType::get( Context, 0 ) ), ExceptionValueSlot() ) );
    static_cast<void>( Builder.CreateStore( Builder.getFalse(), BreakFlagSlot() ) );

    Emitter.EmitStmts( Node.EnsureBody, false );

    if ( not Emitter.Terminated() )
    {
        static_cast<void>( Builder.CreateStore( SavedTag, ExceptionTagSlot() ) );
        static_cast<void>( Builder.CreateStore( SavedExc, ExceptionValueSlot() ) );
        static_cast<void>( Builder.CreateStore( SavedBrk, BreakFlagSlot() ) );

        // A `break` in flight through this `begin` is exactly as "still pending"
        // as an unhandled exception — dispatch above only ever matches exception
        // ancestry, so a break sails through every clause untouched
        // (EmitAncestorTest is false whenever the dynamic tag is still
        // NominalId::InvalidValue) and lands here needing the same
        // re-propagation an unhandled exception gets, or it would be silently
        // absorbed by this `begin`/`ensure` and execution would resume normally
        // past it.
        llvm::Value *StillTag        = Builder.CreateLoad( llvm::Type::getInt32Ty( Context ), ExceptionTagSlot(), "exc.tag" );
        llvm::Value *StillExcPending = Builder.CreateICmpNE(
            StillTag, llvm::ConstantInt::get( llvm::Type::getInt32Ty( Context ), Sema::NominalId::InvalidValue ),
            "exc.still_pending" );
        llvm::Value *StillBrk     = Builder.CreateLoad( Builder.getInt1Ty(), BreakFlagSlot(), "brk.still_pending" );
        llvm::Value *StillPending = Builder.CreateOr( StillExcPending, StillBrk, "unwind.still_pending" );

        llvm::BasicBlock *Repropagate = llvm::BasicBlock::Create( Context, "begin.repropagate", Fn );
        static_cast<void>( Builder.CreateCondBr( StillPending, Repropagate, Merge ) );

        Builder.SetInsertPoint( Repropagate );
        EmitPoisonedPath( Emitter );
    }

    Builder.SetInsertPoint( Merge );
    if ( Slot == nullptr )
    {
        return nullptr;
    }
    return Emitter.LoadConverged( Slot, Shape, Layout, "begin" );
}
