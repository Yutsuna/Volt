// RaiseEmitter.cpp — `raise e`.
//
// Store the object into thread-local storage, publish its address and its
// nominal tag, then take the poisoned path. Nothing touches the C stack.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <string>

llvm::Value *
Volt::Backend::Llvm::ExceptionLowering::EmitRaise ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::RaiseExpr &Node )
{
    if ( not Node.Exception.IsValid() )
    {
        static_cast<void>( Emitter.Fail( "llvm: `raise` at expression " + std::to_string( Id.Value ) +
                                         " carries no exception expression — TypeChecker fills one in for every RaiseExpr "
                                         "it accepts, bare or not" ) );
        return nullptr;
    }

    const MiddleEnd::TypeSystem::UnitTypes &Values = *Emitter.Frame().Values;
    const MiddleEnd::TypeSystem::SemaTypeId Ty     = Values.ExprType( Node.Exception );
    if ( not Values.Has( Ty ) or not Values.Get( Ty ).Base.IsValid() )
    {
        static_cast<void>(
            Emitter.Fail( "llvm: `raise` at expression " + std::to_string( Id.Value ) + " has no resolved exception type" ) );
        return nullptr;
    }
    const MiddleEnd::TypeSystem::NominalId Nominal = Values.Get( Ty ).Base;

    // An exception object is an aggregate like any other, hence an address
    // (abi.md); it is *this* address that both backend and (if the object
    // survives past this function returning) the ambient allocator make valid.
    llvm::Value *ExcPtr = Emitter.EmitExpr( Node.Exception );
    if ( ExcPtr == nullptr )
    {
        return nullptr;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::IRBuilder<> &Builder = Services->Ctx->Builder();

    // Out of the frame before that frame returns. Tier 1 unwinds by returning,
    // so publishing `ExcPtr` itself would hand every `rescue` — and the
    // last-resort hook, which runs with no Volt frame left at all — an address in
    // dead storage. The copy is one memcpy on a path that is already exceptional,
    // and it is what makes "the object outlives the raise" true rather than
    // true-in-practice.
    llvm::Value *Storage                        = ExceptionStorageSlot( Builder );
    const MiddleEnd::TypeSystem::LayoutId Shape = Emitter.LayoutOfExpr( Node.Exception );
    if ( Services->Layouts->has_value() and Shape.IsValid() and ( *Services->Layouts )->Of( Shape ).Size > ExcStorageSize )
    {
        static_cast<void>( Emitter.Fail(
            "llvm: the raised object needs " + std::to_string( ( *Services->Layouts )->Of( Shape ).Size ) +
            " bytes but volt.exc.storage holds " + std::to_string( ExcStorageSize ) +
            " — the buffer is sized for every descendant of the @[Literal( RaiseExpr )], so this type is not one of them" ) );
        return nullptr;
    }
    Emitter.EmitStore( Storage, ExcPtr, Shape );

    static_cast<void>( Builder.CreateStore( Storage, ExceptionValueSlot( Builder ) ) );
    static_cast<void>( Builder.CreateStore( llvm::ConstantInt::get( llvm::Type::getInt32Ty( Context ), Nominal.Value ),
                                            ExceptionTagSlot( Builder ) ) );

    EmitPoisonedPath( Emitter );
    // The block is now terminated; nothing here has a value to hand back, and
    // every caller of EmitExpr already treats a null value as "stop" — the same
    // reading a Fail() return gets, without this being one.
    return nullptr;
}
