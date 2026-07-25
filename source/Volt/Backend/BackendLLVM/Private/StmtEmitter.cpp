// StmtEmitter.cpp — the statement category, and the tail rule that makes an
// implicit return work without any type inference.
//
// Volt has no `return` requirement: `Int8#<=>` ends in an if/elsif/else chain
// and its value is the value of whichever branch ran. That is a *structural*
// property, not a typing one, so it is decided here by position alone:
//
//   EmitStmts( List, bTail ) marks the last statement of a list as being in
//   result position, and only two node kinds do anything with it — an
//   ExprStmt emits `ret`, and an `If` passes it on to both of its branches.
//
// No other node propagates it, because no other node's value can be the
// function's: a `while` has none, and a `return` already is one. The rule
// therefore needs nothing from Sema, which is what lets it live in a backend
// at all — see .agents/backend/llvm.md for why it arguably belongs in a
// Lowering instead.
//
// One `std::visit` for the whole category, like ExprEmitter: a statement the
// contract says cannot reach a backend falls into the `auto` arm and is
// reported by name.

#include "LlvmState.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>

#include <string>
#include <variant>

bool Volt::Backend::Llvm::LlvmBackend::State::Terminated () const
{
    llvm::BasicBlock *Block = Builder->GetInsertBlock();
    return Block == nullptr or Block->getTerminator() != nullptr;
}

llvm::AllocaInst *Volt::Backend::Llvm::LlvmBackend::State::MakeTemp ( llvm::Type *Shape, std::string_view Name ) const
{
    if ( Shape == nullptr or Frame.Entry == nullptr )
    {
        return nullptr;
    }

    // Every alloca goes in the entry block, whatever block the walk is in when
    // it needs one: that is the precondition mem2reg promotes on, and it is why
    // this emitter never builds SSA itself.
    llvm::IRBuilder<> Entry{ Frame.Entry, Frame.Entry->begin() };
    return Entry.CreateAlloca( Shape, nullptr, Name );
}

llvm::Value *
Volt::Backend::Llvm::LlvmBackend::State::SlotFor ( const Sema::BindingSite &Site, llvm::Type *Shape, std::string_view Name )
{
    if ( const auto It = Frame.Slots.find( Site ); It != Frame.Slots.end() )
    {
        return It->second;
    }

    llvm::AllocaInst *Slot = MakeTemp( Shape, Name );
    if ( Slot != nullptr )
    {
        Frame.Slots.emplace( Site, Slot );
    }
    return Slot;
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitStmts ( const Frontend::StmtList &List, bool bTail )
{
    for ( std::size_t Index = 0; Index < List.Size(); ++Index )
    {
        if ( Failed() or Terminated() )
        {
            return;
        }
        EmitStmt( List[Index], bTail and Index + 1 == List.Size() );
    }
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitStmt ( Frontend::StmtId Id, bool bTail )
{
    if ( Failed() or Frame.Unit == nullptr or not Id.IsValid() )
    {
        return;
    }

    const Frontend::AstContext &Ast = *Frame.Unit->Ast;

    std::visit(
        Meta::Overloaded{
            [this, bTail] ( const Frontend::ExprStmt &Node )
            {
                // The tail rule, in one place. Outside result position the
                // value is genuinely discarded — a call for its effect.
                if ( not bTail or not Frame.bReturnsValue )
                {
                    static_cast<void>( EmitExpr( Node.Expr ) );
                    return;
                }

                llvm::Value *Value = EmitExpr( Node.Expr );
                if ( Value == nullptr )
                {
                    return;
                }
                static_cast<void>( Builder->CreateRet( CoerceWidth( Value, Frame.Fn->getReturnType() ) ) );
            },
            [this, bTail] ( const Frontend::If &Node )
            {
                llvm::Value *Cond = EmitExpr( Node.Cond );
                if ( Cond == nullptr )
                {
                    return;
                }

                const bool bHasElse     = Node.Else.Size() > 0;
                llvm::BasicBlock *Then  = llvm::BasicBlock::Create( Context, "if.then", Frame.Fn );
                llvm::BasicBlock *Else  = bHasElse ? llvm::BasicBlock::Create( Context, "if.else", Frame.Fn ) : nullptr;
                llvm::BasicBlock *Merge = llvm::BasicBlock::Create( Context, "if.end", Frame.Fn );
                static_cast<void>( Builder->CreateCondBr( Cond, Then, bHasElse ? Else : Merge ) );

                // Both branches inherit the tail flag: an `elsif` chain is a
                // nested If in the Else branch (Stmt.hpp), so passing it on is
                // exactly what makes the whole chain a result.
                Builder->SetInsertPoint( Then );
                EmitStmts( Node.Then, bTail );
                if ( not Terminated() )
                {
                    static_cast<void>( Builder->CreateBr( Merge ) );
                }

                if ( bHasElse )
                {
                    Builder->SetInsertPoint( Else );
                    EmitStmts( Node.Else, bTail );
                    if ( not Terminated() )
                    {
                        static_cast<void>( Builder->CreateBr( Merge ) );
                    }
                }

                Builder->SetInsertPoint( Merge );
            },
            [this] ( const Frontend::While &Node )
            {
                // The condition gets its own block rather than being emitted
                // into the entry: `next` branches to it, and it is re-evaluated
                // on every iteration.
                llvm::BasicBlock *Test  = llvm::BasicBlock::Create( Context, "while.cond", Frame.Fn );
                llvm::BasicBlock *Body  = llvm::BasicBlock::Create( Context, "while.body", Frame.Fn );
                llvm::BasicBlock *Merge = llvm::BasicBlock::Create( Context, "while.end", Frame.Fn );

                static_cast<void>( Builder->CreateBr( Test ) );
                Builder->SetInsertPoint( Test );
                llvm::Value *Cond = EmitExpr( Node.Cond );
                if ( Cond == nullptr )
                {
                    return;
                }
                static_cast<void>( Builder->CreateCondBr( Cond, Body, Merge ) );

                Frame.Loops.push_back( LoopFrame{ .Latch = Test, .Merge = Merge } );
                Builder->SetInsertPoint( Body );
                EmitStmts( Node.Body, false );
                if ( not Terminated() )
                {
                    static_cast<void>( Builder->CreateBr( Test ) );
                }
                Frame.Loops.pop_back();

                Builder->SetInsertPoint( Merge );
            },
            [this] ( const Frontend::Return &Node )
            {
                if ( not Node.Value.IsValid() )
                {
                    static_cast<void>( Builder->CreateRetVoid() );
                    return;
                }

                llvm::Value *Value = EmitExpr( Node.Value );
                if ( Value == nullptr )
                {
                    return;
                }

                // A `return v` in a function the signature says returns nothing
                // is a middle-end disagreement, not something to silently drop:
                // one of the two read a different signature.
                if ( Frame.Fn->getReturnType()->isVoidTy() )
                {
                    static_cast<void>( Fail( "llvm: `return` with a value in '" + Frame.Fn->getName().str() +
                                             "', whose signature declares no result" ) );
                    return;
                }
                static_cast<void>( Builder->CreateRet( CoerceWidth( Value, Frame.Fn->getReturnType() ) ) );
            },
            [this, Id] ( const Frontend::Break &Node )
            {
                if ( Frame.Loops.empty() )
                {
                    // Inside a closure with no loop of its own, `break` leaves
                    // the *iterating* method — a non-local exit out of a frame
                    // this one does not own. That needs an unwinding transport,
                    // which is the exception emitter's, not a branch.
                    static_cast<void>( Fail( Frame.bClosure
                                                 ? "llvm: `break` inside a closure body is a non-local exit from the method "
                                                   "that invoked it, which needs the exception transport (backend phase 6)"
                                                 : "llvm: `break` outside a loop reached codegen" ) );
                    return;
                }
                // `break v` yields a value out of a loop, and a `while` has no
                // value to yield it as.
                if ( Node.Value.IsValid() )
                {
                    static_cast<void>( Fail( "llvm: `break` with a value at statement " + std::to_string( Id.Value ) +
                                             " — a `while` has no value to yield it as" ) );
                    return;
                }
                static_cast<void>( Builder->CreateBr( Frame.Loops.back().Merge ) );
            },
            [this, Id] ( const Frontend::Next &Node )
            {
                // In a closure with no enclosing loop, `next` *is* the block's
                // result: it ends this invocation, which is a `ret`, not a
                // branch. The same word means "continue" only when there is a
                // loop in this frame to continue.
                if ( Frame.Loops.empty() and Frame.bClosure )
                {
                    EmitBlockNext( Node.Value );
                    return;
                }
                if ( Frame.Loops.empty() )
                {
                    static_cast<void>( Fail( "llvm: `next` outside a loop reached codegen" ) );
                    return;
                }
                if ( Node.Value.IsValid() )
                {
                    static_cast<void>( Fail( "llvm: `next` with a value at statement " + std::to_string( Id.Value ) +
                                             " — a `while` iteration has no value to carry" ) );
                    return;
                }
                static_cast<void>( Builder->CreateBr( Frame.Loops.back().Latch ) );
            },
            [this, Id] ( const Frontend::LocalDecl &Node )
            {
                // The local's shape is the one TypeChecker recorded *for the
                // binding site*, not the initialiser's: a declared
                // `x : UInt64 = 0` is storage of the declared width, and the
                // initialiser widens into it.
                const Sema::BindingSite Site{ Id };
                const Sema::LayoutId Shape = LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Site ) );
                llvm::Type *Slot           = TypeOfLayout( Shape );
                if ( Slot == nullptr )
                {
                    static_cast<void>( Fail( "llvm: local '" + std::string( Frame.Unit->Ast->Text( Node.Name ) ) +
                                             "' has no resolved layout" ) );
                    return;
                }

                llvm::Value *Address = SlotFor( Site, Slot, Frame.Unit->Ast->Text( Node.Name ) );
                if ( Address == nullptr or not Node.Init.IsValid() )
                {
                    return;
                }

                llvm::Value *Value = EmitExpr( Node.Init );
                if ( Value == nullptr )
                {
                    return;
                }
                EmitStore( Address, IsAggregate( Shape ) ? Value : CoerceWidth( Value, Slot ), Shape );
            },
            [this] ( const Frontend::RescueClause & )
            { static_cast<void>( Fail( "llvm: a `rescue` clause needs the exception emitter (backend phase 6)" ) ); },
            [this, Id] ( const auto & )
            {
                static_cast<void>( Fail( "llvm: " + std::string( Frontend::NodeName( Frame.Unit->Ast->Stmt( Id ) ) ) +
                                         " is not a statement this backend emits" ) );
            } },
        Ast.Stmt( Id ) );
}
