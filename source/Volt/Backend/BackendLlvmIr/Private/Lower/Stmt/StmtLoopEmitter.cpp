// StmtLoopEmitter.cpp — `while`, including its post-test form.

#include "Lower/Stmt/StmtEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>

void Volt::Backend::Llvm::EmitWhile ( BodyEmitter &Emitter, const Frontend::While &Node )
{
    FunctionFrame &Frame       = Emitter.Frame();
    llvm::LLVMContext &Context = Emitter.Ctx().Context();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();

    // The condition gets its own block rather than being emitted into the
    // entry: `next` branches to it, and it is re-evaluated on every iteration.
    llvm::BasicBlock *Test  = llvm::BasicBlock::Create( Context, "while.cond", Frame.Fn );
    llvm::BasicBlock *Body  = llvm::BasicBlock::Create( Context, "while.body", Frame.Fn );
    llvm::BasicBlock *Merge = llvm::BasicBlock::Create( Context, "while.end", Frame.Fn );

    // A post-test loop — `begin ... end while c` — differs by one edge: entry
    // goes to the body instead of the test, so the body runs once before the
    // condition is ever evaluated. The LoopFrame is unchanged, which is the
    // point of carrying a flag rather than desugaring: `next` still branches to
    // Test.
    static_cast<void>( Builder.CreateBr( Node.bPostTest ? Body : Test ) );
    Builder.SetInsertPoint( Test );
    llvm::Value *Cond = Emitter.EmitExpr( Node.Cond );
    if ( Cond == nullptr )
    {
        return;
    }
    static_cast<void>( Builder.CreateCondBr( Cond, Body, Merge ) );

    Frame.Loops.push_back( LoopFrame{ .Latch = Test, .Merge = Merge } );
    Builder.SetInsertPoint( Body );
    Emitter.EmitStmts( Node.Body, false );
    if ( not Emitter.Terminated() )
    {
        static_cast<void>( Builder.CreateBr( Test ) );
    }
    Frame.Loops.pop_back();

    Builder.SetInsertPoint( Merge );
}
