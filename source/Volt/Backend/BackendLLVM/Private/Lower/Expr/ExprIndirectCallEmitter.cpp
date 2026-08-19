// ExprIndirectCallEmitter.cpp — the `bIndirect` arm of a resolved call.
//
// The emission itself is ClosureLowering's (Lower/Closure/IndirectCallEmitter.cpp):
// it is the `{ code, env }` calling protocol, and the VM and WASM backends will
// each want their own. What belongs to the *expression* layer, and lives here,
// is the one syntactic rule about it — a trailing `do ... end` block binds to a
// declared `&block` parameter, and a callable's type carries positional
// arguments only, so there is no slot for one to bind to.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/Closure/ClosureLowering.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <string>
#include <vector>

namespace
{

llvm::Value *EmitDynamicMethodCall ( Volt::Backend::Llvm::BodyEmitter &Emitter,
                                     Volt::Frontend::ExprId,
                                     const Volt::MiddleEnd::IR::CalleeEntry &Entry,
                                     Volt::Frontend::ExprId Receiver,
                                     std::span<const Volt::Frontend::ExprId> Args )
{
    llvm::Value *DynamicPair = Emitter.EmitExpr( Receiver );
    if ( DynamicPair == nullptr )
    {
        return nullptr;
    }

    llvm::LLVMContext &Context = Emitter.Ctx().Context();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();
    llvm::Type *PtrTy          = llvm::PointerType::get( Context, 0 );
    llvm::StructType *PairType = llvm::StructType::get( Context, { PtrTy, PtrTy }, false );

    llvm::Value *DataPtrVal =
        Builder.CreateLoad( PtrTy, Builder.CreateStructGEP( PairType, DynamicPair, 0, "dyn.data.ptr" ), "dyn.data" );
    llvm::Value *VTableVal =
        Builder.CreateLoad( PtrTy, Builder.CreateStructGEP( PairType, DynamicPair, 1, "dyn.vtable.ptr" ), "dyn.vtable" );

    llvm::Value *SlotIndex = Builder.getInt32( Entry.VTableSlot );
    llvm::Value *SlotPtr = Builder.CreateGEP( PtrTy, VTableVal, SlotIndex, "vtable.slot." + std::to_string( Entry.VTableSlot ) );
    llvm::Value *FnPtr   = Builder.CreateLoad( PtrTy, SlotPtr, "dyn.fn.ptr" );

    std::vector<llvm::Type *> ParamTypes;
    std::vector<llvm::Value *> CallArgs;
    ParamTypes.push_back( PtrTy );
    CallArgs.push_back( DataPtrVal );

    for ( std::size_t Index = 0; Index < Args.size(); ++Index )
    {
        llvm::Value *ArgVal = Emitter.EmitExpr( Args[Index] );
        if ( ArgVal == nullptr )
        {
            return nullptr;
        }
        ParamTypes.push_back( ArgVal->getType() );
        CallArgs.push_back( ArgVal );
    }

    llvm::Type *RetType = PtrTy;
    if ( Entry.Result.IsValid() and Emitter.Frame().Values != nullptr )
    {
        const auto Layout = Emitter.Types().LayoutOfValue( *Emitter.Frame().Values, Entry.Result );
        if ( Layout.IsValid() )
        {
            llvm::Type *ResolvedRet = Emitter.Types().TypeOfLayout( Layout );
            if ( ResolvedRet != nullptr )
            {
                RetType = ResolvedRet;
            }
        }
    }

    llvm::FunctionType *FnType = llvm::FunctionType::get( RetType, ParamTypes, false );
    return Builder.CreateCall( FnType, FnPtr, CallArgs );
}

} // namespace

llvm::Value *Volt::Backend::Llvm::EmitIndirectDispatch ( BodyEmitter &Emitter,
                                                         Frontend::ExprId Id,
                                                         const MiddleEnd::IR::CalleeEntry &Entry,
                                                         Frontend::ExprId Receiver,
                                                         std::span<const Frontend::ExprId> Args,
                                                         Frontend::ExprId Block )
{
    if ( Block.IsValid() )
    {
        static_cast<void>( Emitter.Fail( "llvm: the call at expression " + std::to_string( Id.Value ) +
                                         " passes a trailing block to a callable, whose type carries positional "
                                         "arguments only" ) );
        return nullptr;
    }
    if ( Entry.bDynamicDispatch )
    {
        return EmitDynamicMethodCall( Emitter, Id, Entry, Receiver, Args );
    }
    return Emitter.Services().Closures->EmitIndirectCall( Emitter, Id, Entry, Receiver, Args );
}
