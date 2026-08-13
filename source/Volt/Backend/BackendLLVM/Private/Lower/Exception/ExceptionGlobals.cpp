// ExceptionGlobals.cpp — the three thread-local slots the transport uses.
//
// Thread-local because the transport is per-thread state, not a value carried by
// any signature: abi.md reserves no channel for it, which is exactly what makes
// tier 1 cost the calling convention nothing.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

llvm::GlobalVariable *Volt::Backend::Llvm::ExceptionLowering::ExceptionValueSlot ()
{
    if ( ExcValue != nullptr )
    {
        return ExcValue;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Type *Address        = llvm::PointerType::get( Context, 0 );
    ExcValue = new llvm::GlobalVariable( Services->Ctx->Mod(), Address, false, llvm::GlobalValue::LinkOnceODRLinkage,
                                         llvm::Constant::getNullValue( Address ), "volt.exc.value" );
    ExcValue->setThreadLocal( true );
    return ExcValue;
}

llvm::GlobalVariable *Volt::Backend::Llvm::ExceptionLowering::ExceptionTagSlot ()
{
    if ( ExcTag != nullptr )
    {
        return ExcTag;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Type *Int32Ty        = llvm::Type::getInt32Ty( Context );
    ExcTag = new llvm::GlobalVariable( Services->Ctx->Mod(), Int32Ty, false, llvm::GlobalValue::LinkOnceODRLinkage,
                                       llvm::ConstantInt::get( Int32Ty, MiddleEnd::TypeSystem::NominalId::InvalidValue ),
                                       "volt.exc.tag" );
    ExcTag->setThreadLocal( true );
    return ExcTag;
}

llvm::GlobalVariable *Volt::Backend::Llvm::ExceptionLowering::BreakFlagSlot ()
{
    if ( BrkFlag != nullptr )
    {
        return BrkFlag;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Type *BoolTy         = llvm::Type::getInt1Ty( Context );
    BrkFlag = new llvm::GlobalVariable( Services->Ctx->Mod(), BoolTy, false, llvm::GlobalValue::LinkOnceODRLinkage,
                                        llvm::ConstantInt::getFalse( BoolTy ), "volt.brk.flag" );
    BrkFlag->setThreadLocal( true );
    return BrkFlag;
}
