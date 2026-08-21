// ExprDynamicUpcast.cpp — `Concrete` in trait position.
//
// A trait object is the pair `{ ptr data, ptr vtable }`, built here and handed
// back by address like every other aggregate (abi.md). Neither half is decided
// in this file: the data pointer is whatever the operand already evaluated to,
// and the vtable is VTableRegistry's, keyed on the (concrete, trait) pair that
// the middle-end stamped on the two SemaTypeIds. What is left is the store of
// two pointers into one temporary.
//
// Every early return here is a middle-end fact that should exist and does not.
// They are silent — nullptr, no diagnostic — because the caller's own arm
// reports the miss with the expression's identity attached, which is the thing
// worth naming.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Functions/VTableRegistry.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>

llvm::Value *
Volt::Backend::Llvm::EmitDynamicUpcast ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::DynamicUpcast &Node )
{
    llvm::Value *DataPtr = Emitter.EmitExpr( Node.Value );
    if ( DataPtr == nullptr or Emitter.Frame().Values == nullptr )
    {
        return nullptr;
    }

    const auto &Values    = *Emitter.Frame().Values;
    const auto ValType    = Values.ExprType( Node.Value );
    const auto UpcastType = Values.ExprType( Id );
    if ( not Values.Has( ValType ) or not Values.Has( UpcastType ) )
    {
        return nullptr;
    }

    const auto ConcreteNominal = Values.Get( ValType ).Base;
    const auto &UpcastVal      = Values.Get( UpcastType );
    if ( UpcastVal.Args.IsEmpty() or not UpcastVal.Args[0].IsValid() )
    {
        return nullptr;
    }
    const auto TraitNominal = Values.Get( UpcastVal.Args[0] ).Base;

    llvm::LLVMContext &Context = Emitter.Ctx().Context();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();
    llvm::Type *PtrTy          = llvm::PointerType::get( Context, 0 );
    llvm::StructType *PairType = llvm::StructType::get( Context, { PtrTy, PtrTy }, false );

    llvm::GlobalVariable *VTable = Emitter.Services().VTables->GetOrCreateVTable( ConcreteNominal, TraitNominal );
    llvm::AllocaInst *Temp       = Emitter.MakeTemp( PairType, "dyn.upcast" );
    if ( Temp == nullptr )
    {
        return nullptr;
    }

    static_cast<void>( Builder.CreateStore( DataPtr, Builder.CreateStructGEP( PairType, Temp, 0, "dyn.data" ) ) );
    static_cast<void>( Builder.CreateStore( VTable, Builder.CreateStructGEP( PairType, Temp, 1, "dyn.vtable" ) ) );
    return Temp;
}
