// AncestorTest.cpp — `is the raised type a descendant of this clause's filter`,
// answered in O(1) at runtime via Euler tour interval testing.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

llvm::Value *Volt::Backend::Llvm::ExceptionLowering::EmitAncestorTest ( BodyEmitter &,
                                                                        llvm::Value *Dynamic,
                                                                        MiddleEnd::TypeSystem::NominalId Target )
{
    MiddleEnd::TypeSystem::TypeStore &Store = *Services->Build->Types;
    const auto [TargetLeft, TargetRight]    = Store.SubtypeInterval( Target );

    llvm::LLVMContext &Context  = Services->Ctx->Context();
    llvm::IRBuilder<> &Builder  = Services->Ctx->Builder();
    llvm::Type *Int32Ty         = llvm::Type::getInt32Ty( Context );
    llvm::GlobalVariable *Table = PreorderTable();

    // Guard against out-of-bounds / InvalidValue tag:
    llvm::Value *CountConst = llvm::ConstantInt::get( Int32Ty, Store.TypeCount() );
    llvm::Value *IsValidTag = Builder.CreateICmpULT( Dynamic, CountConst, "tag.valid" );
    llvm::Value *SafeIdx    = Builder.CreateSelect( IsValidTag, Dynamic, llvm::ConstantInt::get( Int32Ty, 0 ), "dyn.idx" );

    // 1. Load Dynamic type's Left number from volt.type.preorder[SafeIdx]
    llvm::Value *DynLAddr = Builder.CreateGEP( Int32Ty, Table, { SafeIdx }, "dyn.l.addr" );
    llvm::Value *DynLeft  = Builder.CreateLoad( Int32Ty, DynLAddr, "dyn.l" );

    // 2. O(1) interval test: TargetLeft <= DynLeft && DynLeft <= TargetRight
    llvm::Value *Ge      = Builder.CreateICmpUGE( DynLeft, llvm::ConstantInt::get( Int32Ty, TargetLeft ), "isa.ge" );
    llvm::Value *Le      = Builder.CreateICmpULE( DynLeft, llvm::ConstantInt::get( Int32Ty, TargetRight ), "isa.le" );
    llvm::Value *InRange = Builder.CreateAnd( Ge, Le, "isa.in_range" );

    return Builder.CreateAnd( IsValidTag, InRange, "ancestor.result" );
}
