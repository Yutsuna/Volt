// ExceptionChecks.cpp — the post-call checks.
//
// Two of them, and the difference is the whole of the `break` protocol:
//
//   EmitExceptionCheck  tests the exception tag only. Read by exactly one call
//                       site — a call that bound a trailing `do…end` — because
//                       *that* site is where a `break` inside the block is
//                       consumed rather than propagated further.
//   EmitUnwindCheck     tests the tag *or* the break flag. Every other call
//                       site, including `block.call(...)`, which must let a
//                       `break` the block contains keep unwinding through it.
//
// Getting these two the wrong way round is silent: the program simply resumes
// past a `break` that should have left the frame.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

void Volt::Backend::Llvm::ExceptionLowering::EmitExceptionCheck ( BodyEmitter &Emitter )
{
    if ( Emitter.Failed() or Emitter.Terminated() )
    {
        return;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::IRBuilder<> &Builder = Services->Ctx->Builder();
    llvm::Function *Fn         = Emitter.Frame().Fn;

    llvm::Type *Int32Ty = llvm::Type::getInt32Ty( Context );
    llvm::Value *Tag    = Builder.CreateLoad( Int32Ty, ExceptionTagSlot(), "exc.tag" );
    llvm::Value *Pending =
        Builder.CreateICmpNE( Tag, llvm::ConstantInt::get( Int32Ty, Sema::NominalId::InvalidValue ), "exc.pending" );

    llvm::BasicBlock *Propagate = llvm::BasicBlock::Create( Context, "exc.propagate", Fn );
    llvm::BasicBlock *Continue  = llvm::BasicBlock::Create( Context, "exc.cont", Fn );
    static_cast<void>( Builder.CreateCondBr( Pending, Propagate, Continue ) );

    Builder.SetInsertPoint( Propagate );
    EmitPoisonedPath( Emitter );

    Builder.SetInsertPoint( Continue );
}

void Volt::Backend::Llvm::ExceptionLowering::EmitUnwindCheck ( BodyEmitter &Emitter )
{
    if ( Emitter.Failed() or Emitter.Terminated() )
    {
        return;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::IRBuilder<> &Builder = Services->Ctx->Builder();
    llvm::Function *Fn         = Emitter.Frame().Fn;

    llvm::Type *Int32Ty = llvm::Type::getInt32Ty( Context );
    llvm::Value *Tag    = Builder.CreateLoad( Int32Ty, ExceptionTagSlot(), "exc.tag" );
    llvm::Value *ExcPending =
        Builder.CreateICmpNE( Tag, llvm::ConstantInt::get( Int32Ty, Sema::NominalId::InvalidValue ), "exc.pending" );
    llvm::Value *Brk     = Builder.CreateLoad( Builder.getInt1Ty(), BreakFlagSlot(), "brk.pending" );
    llvm::Value *Pending = Builder.CreateOr( ExcPending, Brk, "unwind.pending" );

    llvm::BasicBlock *Propagate = llvm::BasicBlock::Create( Context, "unwind.propagate", Fn );
    llvm::BasicBlock *Continue  = llvm::BasicBlock::Create( Context, "unwind.cont", Fn );
    static_cast<void>( Builder.CreateCondBr( Pending, Propagate, Continue ) );

    Builder.SetInsertPoint( Propagate );
    EmitPoisonedPath( Emitter );

    Builder.SetInsertPoint( Continue );
}
