// AncestorTest.cpp — `is the raised type a descendant of this clause's filter`,
// answered at runtime.
//
// The same reflexive, depth-bounded walk IsSubclassOf performs in Sema, done
// here as a loop over AncestryTable's rows because only the *dynamic* side of
// the test is unknown at compile time.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

// NOLINTBEGIN(clang-analyzer-security.ArrayBound) — false positive via LLVM's hung-off operand layout
llvm::Value *Volt::Backend::Llvm::ExceptionLowering::EmitAncestorTest ( BodyEmitter &Emitter,
                                                                        llvm::Value *Dynamic,
                                                                        MiddleEnd::TypeSystem::NominalId Target )
{
    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::IRBuilder<> &Builder = Services->Ctx->Builder();
    llvm::Function *Fn         = Emitter.Frame().Fn;

    llvm::Type *Int32Ty         = llvm::Type::getInt32Ty( Context );
    llvm::Value *TargetConst    = llvm::ConstantInt::get( Int32Ty, Target.Value );
    llvm::Value *SentinelConst  = llvm::ConstantInt::get( Int32Ty, MiddleEnd::TypeSystem::NominalId::InvalidValue );
    llvm::GlobalVariable *Table = AncestryTable();
    llvm::BasicBlock *Preheader = Builder.GetInsertBlock();

    llvm::BasicBlock *Loop      = llvm::BasicBlock::Create( Context, "ancestor.loop", Fn );
    llvm::BasicBlock *CheckNone = llvm::BasicBlock::Create( Context, "ancestor.none", Fn );
    llvm::BasicBlock *Step      = llvm::BasicBlock::Create( Context, "ancestor.step", Fn );
    llvm::BasicBlock *Done      = llvm::BasicBlock::Create( Context, "ancestor.done", Fn );

    static_cast<void>( Builder.CreateBr( Loop ) );

    // `cur == Target` -> a hit; `cur == InvalidValue` -> walked off the root with
    // no match; otherwise step to `cur`'s own Super and try again. Bounded by
    // construction: the table is acyclic (a class hierarchy is a tree), so this
    // always reaches one of the two exits.
    Builder.SetInsertPoint( Loop );
    llvm::PHINode *Current = Builder.CreatePHI( Int32Ty, 2, "ancestor.cur" );
    Current->addIncoming( Dynamic, Preheader );
    llvm::Value *IsMatch = Builder.CreateICmpEQ( Current, TargetConst, "ancestor.match" );
    static_cast<void>( Builder.CreateCondBr( IsMatch, Done, CheckNone ) );

    Builder.SetInsertPoint( CheckNone );
    llvm::Value *IsNone = Builder.CreateICmpEQ( Current, SentinelConst, "ancestor.isnone" );
    static_cast<void>( Builder.CreateCondBr( IsNone, Done, Step ) );

    Builder.SetInsertPoint( Step );
    llvm::Value *Addr = Builder.CreateGEP( Int32Ty, Table, { Current }, "ancestor.addr" );
    llvm::Value *Next = Builder.CreateLoad( Int32Ty, Addr, "ancestor.next" );
    static_cast<void>( Builder.CreateBr( Loop ) );
    Current->addIncoming( Next, Step );

    Builder.SetInsertPoint( Done );
    llvm::PHINode *Result = Builder.CreatePHI( Builder.getInt1Ty(), 2, "ancestor.result" );
    Result->addIncoming( Builder.getTrue(), Loop );
    Result->addIncoming( Builder.getFalse(), CheckNone );
    return Result;
}
// NOLINTEND(clang-analyzer-security.ArrayBound)
