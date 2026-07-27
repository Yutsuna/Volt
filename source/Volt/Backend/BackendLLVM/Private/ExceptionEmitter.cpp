// ExceptionEmitter.cpp — `RaiseExpr` / `BeginExpr`, Tier 1.
//
// backend/llvm.md: "setjmp/sigsetjmp-free personality-less unwinding is not
// attempted". `raise` never touches the C stack directly — it stores the
// exception into thread-local state and takes the *poisoned path*: a branch
// to the innermost `begin` this function owns, or an early return carrying no
// value, exactly as if the raising call itself had simply returned. Every
// ordinary call this emitter makes is followed by a check of that same state
// (EmitExceptionCheck, wired into EmitResolvedCall / EmitApplyCall in
// ExprEmitter.cpp / ClosureEmitter.cpp), so a `raise` several calls deep
// unwinds one `ret` at a time until some caller's frame is inside a `begin`.
// Simple, portable, correct, and — because it never reserves a channel in any
// signature — it costs the calling convention nothing (abi.md).
//
// `rescue` clause matching compares the raised object's own NominalId against
// each clause's *resolved* filter (Values->SiteType(BindingSite{ClauseId}),
// which ExprInferencer now records for every clause, bound variable or not)
// by walking an ancestry table emitted once as module data — the same
// reflexive, depth-bounded walk Sema's own IsSubclassOf performs
// (TypeResolve.cpp), done here at runtime because only the *dynamic* side of
// the test is unknown at compile time.

#include "LlvmState.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>

#include <string>
#include <variant>
#include <vector>

llvm::GlobalVariable *Volt::Backend::Llvm::LlvmBackend::State::ExceptionValueSlot ()
{
    if ( ExcValue != nullptr )
    {
        return ExcValue;
    }

    llvm::Type *Address = llvm::PointerType::get( Context, 0 );
    ExcValue            = new llvm::GlobalVariable( *Mod, Address, false, llvm::GlobalValue::InternalLinkage,
                                                    llvm::Constant::getNullValue( Address ), "volt.exc.value" );
    ExcValue->setThreadLocal( true );
    return ExcValue;
}

llvm::GlobalVariable *Volt::Backend::Llvm::LlvmBackend::State::ExceptionTagSlot ()
{
    if ( ExcTag != nullptr )
    {
        return ExcTag;
    }

    llvm::Type *Int32Ty = llvm::Type::getInt32Ty( Context );
    ExcTag              = new llvm::GlobalVariable( *Mod, Int32Ty, false, llvm::GlobalValue::InternalLinkage,
                                                    llvm::ConstantInt::get( Int32Ty, Sema::NominalId::InvalidValue ), "volt.exc.tag" );
    ExcTag->setThreadLocal( true );
    return ExcTag;
}

namespace
{

// Reflexive and depth-bounded, exactly like the ancestry walk below and like
// Sema's own IsSubclassOf: "is `Id` the root, or does its Super chain reach
// it". The bound is the same one TypeStore::LookupMember uses — a malformed
// cyclic hierarchy must not hang codegen.
[[nodiscard]] bool DescendsFrom ( const Volt::Sema::TypeStore &Store, Volt::Sema::NominalId Id, Volt::Sema::NominalId Root )
{
    for ( std::uint32_t Depth = 0; Id.IsValid() and Depth <= 16; ++Depth )
    {
        if ( Id == Root )
        {
            return true;
        }
        Id = Store.BaseOf( Store.Type( Id ).Super );
    }
    return false;
}

} // namespace

llvm::GlobalVariable *Volt::Backend::Llvm::LlvmBackend::State::ExceptionStorageSlot ()
{
    if ( ExcStorage != nullptr )
    {
        return ExcStorage;
    }

    Sema::TypeStore &Store = *Build->Types;

    // The widest thing that can ever be in flight, measured rather than
    // guessed: every type that descends from the one annotated
    // `@[ExceptionRoot]`, sized by LayoutEngine — the single ABI authority
    // (abi.md). No type name enters, and an empty answer (no root declared, so
    // nothing can be raised at all) still yields a valid one-byte buffer
    // rather than a zero-length global.
    std::size_t Size      = 1;
    std::size_t Alignment = 1;
    if ( const auto Root = Store.GetExceptionRoot(); Root.has_value() and Layouts.has_value() )
    {
        for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
        {
            const Sema::NominalId Id{ static_cast<Sema::NominalId::ValueType>( Index ) };
            const Sema::LayoutId Shape = Store.Type( Id ).Layout;
            if ( not Shape.IsValid() or not DescendsFrom( Store, Id, *Root ) )
            {
                continue;
            }
            const SizeAlign Measured = Layouts->Of( Shape );
            Size                     = std::max( Size, Measured.Size );
            Alignment                = std::max( Alignment, Measured.Alignment );
        }
    }

    llvm::ArrayType *Bytes = llvm::ArrayType::get( llvm::Type::getInt8Ty( Context ), Size );
    ExcStorage             = new llvm::GlobalVariable( *Mod, Bytes, false, llvm::GlobalValue::InternalLinkage,
                                                       llvm::Constant::getNullValue( Bytes ), "volt.exc.storage" );
    ExcStorage->setThreadLocal( true );
    ExcStorage->setAlignment( llvm::Align( Alignment ) );
    ExcStorageSize = Size;
    return ExcStorage;
}

const Volt::Sema::Member *Volt::Backend::Llvm::LlvmBackend::State::UnhandledHook ( Sema::NominalId &OutOwner ) const
{
    if ( Build == nullptr or Build->Types == nullptr )
    {
        return nullptr;
    }
    const Sema::TypeStore &Store = *Build->Types;

    const auto Root = Store.GetExceptionRoot();
    if ( not Root.has_value() )
    {
        return nullptr;
    }

    // Looked up on the root only, and called statically. Volt dispatches
    // methods statically everywhere else too (llvm.md, SuperExpr), so a
    // subclass overriding the hook would not be picked up — routing by the tag
    // would need a per-NominalId dispatch table, which is tier-2 work and
    // buys nothing until Volt has virtual dispatch at all.
    for ( const Sema::Member &Entry : Store.Type( *Root ).Members )
    {
        if ( Entry.Kind == Sema::EMemberKind::Method and Entry.bUnhandled )
        {
            OutOwner = *Root;
            return &Entry;
        }
    }
    return nullptr;
}

llvm::GlobalVariable *Volt::Backend::Llvm::LlvmBackend::State::AncestryTable ()
{
    if ( Ancestry != nullptr )
    {
        return Ancestry;
    }

    Sema::TypeStore &Store  = *Build->Types;
    const std::size_t Count = Store.TypeCount();
    llvm::Type *Int32Ty     = llvm::Type::getInt32Ty( Context );

    // One row per declared type: its immediate Super's NominalId, or
    // NominalId::InvalidValue for a type with none — the same chain
    // IsSubclassOf walks in Sema (TypeResolve.cpp), reused here as data rather
    // than as a function, since the emitter cannot call back into Sema at
    // runtime.
    std::vector<llvm::Constant *> Rows;
    Rows.reserve( Count );
    for ( std::size_t Index = 0; Index < Count; ++Index )
    {
        const Sema::NominalId Id{ static_cast<Sema::NominalId::ValueType>( Index ) };
        const Sema::NominalId Super = Store.BaseOf( Store.Type( Id ).Super );
        Rows.push_back( llvm::ConstantInt::get( Int32Ty, Super.IsValid() ? Super.Value : Sema::NominalId::InvalidValue ) );
    }

    llvm::ArrayType *TableType = llvm::ArrayType::get( Int32Ty, Count );
    Ancestry                   = new llvm::GlobalVariable( *Mod, TableType, true, llvm::GlobalValue::InternalLinkage,
                                                           llvm::ConstantArray::get( TableType, Rows ), "volt.exc.ancestry" );
    return Ancestry;
}

// NOLINTBEGIN(clang-analyzer-security.ArrayBound) — false positive via LLVM's hung-off operand layout
llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitAncestorTest ( llvm::Value *Dynamic, Sema::NominalId Target )
{
    llvm::Type *Int32Ty         = llvm::Type::getInt32Ty( Context );
    llvm::Value *TargetConst    = llvm::ConstantInt::get( Int32Ty, Target.Value );
    llvm::Value *SentinelConst  = llvm::ConstantInt::get( Int32Ty, Sema::NominalId::InvalidValue );
    llvm::GlobalVariable *Table = AncestryTable();
    llvm::BasicBlock *Preheader = Builder->GetInsertBlock();

    llvm::BasicBlock *Loop      = llvm::BasicBlock::Create( Context, "ancestor.loop", Frame.Fn );
    llvm::BasicBlock *CheckNone = llvm::BasicBlock::Create( Context, "ancestor.none", Frame.Fn );
    llvm::BasicBlock *Step      = llvm::BasicBlock::Create( Context, "ancestor.step", Frame.Fn );
    llvm::BasicBlock *Done      = llvm::BasicBlock::Create( Context, "ancestor.done", Frame.Fn );

    static_cast<void>( Builder->CreateBr( Loop ) );

    // `cur == Target` -> a hit; `cur == InvalidValue` -> walked off the root
    // with no match; otherwise step to `cur`'s own Super and try again. Bounded
    // by construction: the table is acyclic (a class hierarchy is a tree), so
    // this always reaches one of the two exits.
    Builder->SetInsertPoint( Loop );
    llvm::PHINode *Current = Builder->CreatePHI( Int32Ty, 2, "ancestor.cur" );
    Current->addIncoming( Dynamic, Preheader );
    llvm::Value *IsMatch = Builder->CreateICmpEQ( Current, TargetConst, "ancestor.match" );
    static_cast<void>( Builder->CreateCondBr( IsMatch, Done, CheckNone ) );

    Builder->SetInsertPoint( CheckNone );
    llvm::Value *IsNone = Builder->CreateICmpEQ( Current, SentinelConst, "ancestor.isnone" );
    static_cast<void>( Builder->CreateCondBr( IsNone, Done, Step ) );

    Builder->SetInsertPoint( Step );
    llvm::Value *Addr = Builder->CreateGEP( Int32Ty, Table, { Current }, "ancestor.addr" );
    llvm::Value *Next = Builder->CreateLoad( Int32Ty, Addr, "ancestor.next" );
    static_cast<void>( Builder->CreateBr( Loop ) );
    Current->addIncoming( Next, Step );

    Builder->SetInsertPoint( Done );
    llvm::PHINode *Result = Builder->CreatePHI( Builder->getInt1Ty(), 2, "ancestor.result" );
    Result->addIncoming( Builder->getTrue(), Loop );
    Result->addIncoming( Builder->getFalse(), CheckNone );
    return Result;
}
// NOLINTEND(clang-analyzer-security.ArrayBound)

void Volt::Backend::Llvm::LlvmBackend::State::EmitPoisonedPath ()
{
    if ( Terminated() )
    {
        return;
    }

    // A `begin` owned by *this* function catches first; nothing further up the
    // AST does, since Tier 1 never crosses a call boundary except by this
    // early-return protocol — the caller's own post-call check is what
    // continues the unwind one frame further.
    if ( not Frame.Rescues.empty() )
    {
        static_cast<void>( Builder->CreateBr( Frame.Rescues.back() ) );
        return;
    }

    if ( Frame.bReturnsValue )
    {
        // Volt has no divergent/bottom type to make "this never really
        // returns" a typing question (rules/core-ast.md lists the closest
        // relative: a value-returning body falling off its end), so the
        // poisoned value is simply the type's zero — the caller's post-call
        // check reads the thread-local state, never this value.
        static_cast<void>( Builder->CreateRet( llvm::Constant::getNullValue( Frame.Fn->getReturnType() ) ) );
    }
    else
    {
        static_cast<void>( Builder->CreateRetVoid() );
    }
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitExceptionCheck ()
{
    if ( Failed() or Terminated() )
    {
        return;
    }

    llvm::Type *Int32Ty = llvm::Type::getInt32Ty( Context );
    llvm::Value *Tag    = Builder->CreateLoad( Int32Ty, ExceptionTagSlot(), "exc.tag" );
    llvm::Value *Pending =
        Builder->CreateICmpNE( Tag, llvm::ConstantInt::get( Int32Ty, Sema::NominalId::InvalidValue ), "exc.pending" );

    llvm::BasicBlock *Propagate = llvm::BasicBlock::Create( Context, "exc.propagate", Frame.Fn );
    llvm::BasicBlock *Continue  = llvm::BasicBlock::Create( Context, "exc.cont", Frame.Fn );
    static_cast<void>( Builder->CreateCondBr( Pending, Propagate, Continue ) );

    Builder->SetInsertPoint( Propagate );
    EmitPoisonedPath();

    Builder->SetInsertPoint( Continue );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitRaise ( Frontend::ExprId Id, const Frontend::RaiseExpr &Node )
{
    if ( not Node.Exception.IsValid() )
    {
        static_cast<void>( Fail( "llvm: `raise` at expression " + std::to_string( Id.Value ) +
                                 " carries no exception expression — TypeChecker fills one in for every RaiseExpr it "
                                 "accepts, bare or not" ) );
        return nullptr;
    }

    const Sema::UnitTypes &Values = *Frame.Values;
    const Sema::SemaTypeId Ty     = Values.ExprType( Node.Exception );
    if ( not Values.Has( Ty ) or not Values.Get( Ty ).Base.IsValid() )
    {
        static_cast<void>(
            Fail( "llvm: `raise` at expression " + std::to_string( Id.Value ) + " has no resolved exception type" ) );
        return nullptr;
    }
    const Sema::NominalId Nominal = Values.Get( Ty ).Base;

    // An exception object is an aggregate like any other, hence an address
    // (abi.md); it is *this* address that both backend and (if the object
    // survives past this function returning) the ambient allocator make valid.
    llvm::Value *ExcPtr = EmitExpr( Node.Exception );
    if ( ExcPtr == nullptr )
    {
        return nullptr;
    }

    // Out of the frame before that frame returns. Tier 1 unwinds by returning,
    // so publishing `ExcPtr` itself would hand every `rescue` — and the
    // last-resort hook, which runs with no Volt frame left at all — an address
    // in dead storage. The copy is one memcpy on a path that is already
    // exceptional, and it is what makes "the object outlives the raise" true
    // rather than true-in-practice.
    llvm::GlobalVariable *Storage = ExceptionStorageSlot();
    const Sema::LayoutId Shape    = LayoutOfExpr( Node.Exception );
    if ( Layouts.has_value() and Shape.IsValid() and Layouts->Of( Shape ).Size > ExcStorageSize )
    {
        static_cast<void>( Fail( "llvm: the raised object needs " + std::to_string( Layouts->Of( Shape ).Size ) +
                                 " bytes but volt.exc.storage holds " + std::to_string( ExcStorageSize ) +
                                 " — the buffer is sized for every descendant of the @[ExceptionRoot], so this type is "
                                 "not one of them" ) );
        return nullptr;
    }
    EmitStore( Storage, ExcPtr, Shape );

    static_cast<void>( Builder->CreateStore( Storage, ExceptionValueSlot() ) );
    static_cast<void>(
        Builder->CreateStore( llvm::ConstantInt::get( llvm::Type::getInt32Ty( Context ), Nominal.Value ), ExceptionTagSlot() ) );

    EmitPoisonedPath();
    // The block is now terminated; nothing here has a value to hand back, and
    // every caller of EmitExpr already treats a null value as "stop" — the
    // same reading a Fail() return gets, without this being one.
    return nullptr;
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitBeginTail ( const Frontend::StmtList &Body,
                                                              llvm::AllocaInst *Slot,
                                                              llvm::Type *Shape )
{
    const Frontend::AstContext &Ast = *Frame.Unit->Ast;
    for ( std::size_t Index = 0; Index < Body.Size() and not Terminated() and not Failed(); ++Index )
    {
        const bool bLast = Index + 1 == Body.Size();
        if ( bLast and Slot != nullptr )
        {
            if ( const auto *Tail = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Index] ) ); Tail != nullptr )
            {
                llvm::Value *Value = EmitExpr( Tail->Expr );
                if ( Value != nullptr )
                {
                    static_cast<void>( Builder->CreateStore( CoerceWidth( Value, Shape ), Slot ) );
                }
                continue;
            }
        }
        EmitStmt( Body[Index], false );
    }
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitBegin ( Frontend::ExprId Id, const Frontend::BeginExpr &Node )
{
    const Frontend::AstContext &Ast = *Frame.Unit->Ast;
    const Sema::UnitTypes &Values   = *Frame.Values;

    // A `begin` in value position needs a slot for the same reason `case`
    // does: its body and every clause are statement lists, so their results
    // converge through storage rather than a phi over a block count fixed only
    // once the clause ladder is built.
    llvm::Type *Shape      = TypeOfExpr( Id );
    llvm::AllocaInst *Slot = nullptr;
    if ( Shape != nullptr and not IsAggregate( LayoutOfExpr( Id ) ) )
    {
        Slot = MakeTemp( Shape, "begin.result" );
    }

    llvm::BasicBlock *Dispatch = llvm::BasicBlock::Create( Context, "rescue.dispatch", Frame.Fn );
    llvm::BasicBlock *Ensure   = llvm::BasicBlock::Create( Context, "begin.ensure", Frame.Fn );
    llvm::BasicBlock *Merge    = llvm::BasicBlock::Create( Context, "begin.end", Frame.Fn );

    // The body's own raises, and any call inside it that finds one pending,
    // land in Dispatch — this begin gets first refusal.
    Frame.Rescues.push_back( Dispatch );
    EmitBeginTail( Node.Body, Slot, Shape );
    Frame.Rescues.pop_back();
    if ( not Terminated() )
    {
        // Completed with nothing pending: `rescue` never runs, straight to
        // `ensure`.
        static_cast<void>( Builder->CreateBr( Ensure ) );
    }

    Builder->SetInsertPoint( Dispatch );
    if ( Failed() )
    {
        return nullptr;
    }
    llvm::Value *Tag    = Builder->CreateLoad( llvm::Type::getInt32Ty( Context ), ExceptionTagSlot(), "exc.tag" );
    llvm::Value *ExcPtr = Builder->CreateLoad( llvm::PointerType::get( Context, 0 ), ExceptionValueSlot(), "exc.value" );

    // A raise reaching here from inside a *clause's own* body must not re-enter
    // this begin's own clause ladder — it skips straight to `ensure`, which
    // still has to run — so the target changes for exactly the span of the
    // clause bodies below.
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
            static_cast<void>( Fail( "llvm: a `rescue` clause is not a RescueClause" ) );
            Frame.Rescues.pop_back();
            return nullptr;
        }

        // ExprInferencer records this for every clause, bound variable or not
        // — the filter Sema resolved, never re-resolved here.
        const Sema::SemaTypeId FilterType = Values.SiteType( Sema::BindingSite{ ClauseId } );
        if ( not Values.Has( FilterType ) or not Values.Get( FilterType ).Base.IsValid() )
        {
            static_cast<void>( Fail( "llvm: `rescue` clause at statement " + std::to_string( ClauseId.Value ) +
                                     " has no resolved filter type" ) );
            Frame.Rescues.pop_back();
            return nullptr;
        }
        const Sema::NominalId Target = Values.Get( FilterType ).Base;

        llvm::Value *Matches = EmitAncestorTest( Tag, Target );

        llvm::BasicBlock *Body = llvm::BasicBlock::Create( Context, "rescue.body", Frame.Fn );
        llvm::BasicBlock *Next = llvm::BasicBlock::Create( Context, "rescue.next", Frame.Fn );
        static_cast<void>( Builder->CreateCondBr( Matches, Body, Next ) );

        Builder->SetInsertPoint( Body );
        // Handled: clear the in-flight state before running the handler, so a
        // call inside it is checked against whatever *it* raises, never the
        // exception it is currently handling.
        static_cast<void>( Builder->CreateStore(
            llvm::ConstantInt::get( llvm::Type::getInt32Ty( Context ), Sema::NominalId::InvalidValue ), ExceptionTagSlot() ) );
        static_cast<void>(
            Builder->CreateStore( llvm::Constant::getNullValue( llvm::PointerType::get( Context, 0 ) ), ExceptionValueSlot() ) );

        if ( Clause->VarName.IsValid() )
        {
            const Sema::LayoutId FilterShape = LayoutOfValue( Values, FilterType );
            llvm::Type *VarType              = TypeOfLayout( FilterShape );
            if ( VarType == nullptr )
            {
                static_cast<void>( Fail( "llvm: rescue variable '" + std::string( Frame.Unit->Ast->Text( Clause->VarName ) ) +
                                         "' has no resolved layout" ) );
                Frame.Rescues.pop_back();
                return nullptr;
            }
            llvm::Value *VarSlot = SlotFor( Sema::BindingSite{ ClauseId }, VarType, Frame.Unit->Ast->Text( Clause->VarName ) );
            if ( VarSlot == nullptr )
            {
                Frame.Rescues.pop_back();
                return nullptr;
            }
            EmitStore( VarSlot, ExcPtr, FilterShape );
        }

        EmitBeginTail( Clause->Body, Slot, Shape );
        if ( not Terminated() )
        {
            static_cast<void>( Builder->CreateBr( Ensure ) );
        }

        Builder->SetInsertPoint( Next );
        if ( Failed() )
        {
            Frame.Rescues.pop_back();
            return nullptr;
        }
    }

    Frame.Rescues.pop_back();

    // Fell through every clause with nothing matching: still unhandled. The
    // state is left exactly as Dispatch found it, so the recheck after
    // `ensure` below re-propagates it.
    if ( not Terminated() )
    {
        static_cast<void>( Builder->CreateBr( Ensure ) );
    }

    Builder->SetInsertPoint( Ensure );
    EmitStmts( Node.EnsureBody, false );
    if ( not Terminated() )
    {
        llvm::Value *StillTag     = Builder->CreateLoad( llvm::Type::getInt32Ty( Context ), ExceptionTagSlot(), "exc.tag" );
        llvm::Value *StillPending = Builder->CreateICmpNE(
            StillTag, llvm::ConstantInt::get( llvm::Type::getInt32Ty( Context ), Sema::NominalId::InvalidValue ),
            "exc.still_pending" );

        llvm::BasicBlock *Repropagate = llvm::BasicBlock::Create( Context, "begin.repropagate", Frame.Fn );
        static_cast<void>( Builder->CreateCondBr( StillPending, Repropagate, Merge ) );

        Builder->SetInsertPoint( Repropagate );
        EmitPoisonedPath();
    }

    Builder->SetInsertPoint( Merge );
    if ( Slot == nullptr )
    {
        return nullptr;
    }
    return Builder->CreateLoad( Shape, Slot, "begin" );
}
