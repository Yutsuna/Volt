// BodyEmitter.cpp — construction, service access, and the two helpers every
// category shares (entry-block temporaries and the widening coercion).
//
// The per-node emission itself lives in Stmt/, Expr/, Exception/ and Closure/;
// what is here is only what more than one of them needs.

#include "Lower/BodyEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <string>
#include <utility>

Volt::Backend::Llvm::BodyEmitter::BodyEmitter ( EmitterServices &InServices, FunctionFrame &InFrame )
    : ServicesPtr( &InServices ), FramePtr( &InFrame )
{
    // The symbol every diagnostic raised while this body is being emitted gets
    // appended to it. Registered here rather than at ~60 reporting sites, which
    // is what the old `if ( Frame.Fn != nullptr )` inside Fail did.
    if ( FramePtr->Fn != nullptr )
    {
        ServicesPtr->Diag->SetCurrentFunctionName( FramePtr->Fn->getName().str() );
    }
}

Volt::Backend::Llvm::BodyEmitter::~BodyEmitter ()
{
    ServicesPtr->Diag->ClearCurrentFunctionName();
}

Volt::Backend::Llvm::ModuleContext &Volt::Backend::Llvm::BodyEmitter::Ctx () const noexcept
{
    return *ServicesPtr->Ctx;
}

Volt::Backend::Llvm::DiagnosticSink &Volt::Backend::Llvm::BodyEmitter::Diag () const noexcept
{
    return *ServicesPtr->Diag;
}

Volt::Backend::Llvm::TypeMapper &Volt::Backend::Llvm::BodyEmitter::Types () const noexcept
{
    return *ServicesPtr->Types;
}

bool Volt::Backend::Llvm::BodyEmitter::Failed () const noexcept
{
    return ServicesPtr->Diag->Failed();
}

bool Volt::Backend::Llvm::BodyEmitter::Terminated () const noexcept
{
    return ServicesPtr->Ctx->Terminated();
}

Volt::Backend::EEmitStatus Volt::Backend::Llvm::BodyEmitter::Fail ( std::string Message )
{
    return ServicesPtr->Diag->Fail( std::move( Message ) );
}

Volt::MiddleEnd::TypeSystem::LayoutId Volt::Backend::Llvm::BodyEmitter::LayoutOfExpr ( Frontend::ExprId Id ) const
{
    return ServicesPtr->Types->LayoutOfExpr( *FramePtr, Id );
}

llvm::Type *Volt::Backend::Llvm::BodyEmitter::TypeOfExpr ( Frontend::ExprId Id ) const
{
    return ServicesPtr->Types->TypeOfExpr( *FramePtr, Id );
}

bool Volt::Backend::Llvm::BodyEmitter::IsAggregate ( MiddleEnd::TypeSystem::LayoutId Id ) const
{
    return ServicesPtr->Types->IsAggregate( Id );
}

llvm::AllocaInst *Volt::Backend::Llvm::BodyEmitter::MakeTemp ( llvm::Type *Shape, std::string_view Name ) const
{
    if ( Shape == nullptr or FramePtr->Entry == nullptr )
    {
        return nullptr;
    }

    // Every alloca goes in the entry block, whatever block the walk is in when
    // it needs one: that is the precondition mem2reg promotes on, and it is why
    // this emitter never builds SSA itself.
    llvm::IRBuilder<> Entry{ FramePtr->Entry, FramePtr->Entry->begin() };
    return Entry.CreateAlloca( Shape, nullptr, Name );
}

llvm::Value *Volt::Backend::Llvm::BodyEmitter::CoerceWidth ( llvm::Value *Value, llvm::Type *Target ) const
{
    // TypeCompat's IsWideningScalar accepts an `i8` where a `u64` is expected
    // (rules/zero-hardcode.md: Volt has no integer conversion at all, so `hash`
    // would be unwritable without it). That decision was Sema's; here it is
    // simply honoured with the zext the machine needs. Signedness deliberately
    // does not enter the rule, and a zext is what "same family, never
    // narrowing" means on the wire.
    if ( Value == nullptr or Target == nullptr or Value->getType() == Target )
    {
        return Value;
    }
    if ( Value->getType()->isIntegerTy() and Target->isIntegerTy() and
         Value->getType()->getIntegerBitWidth() < Target->getIntegerBitWidth() )
    {
        return ServicesPtr->Ctx->Builder().CreateZExt( Value, Target );
    }
    // An aggregate expression evaluates to a `ptr` at its storage — the
    // convention this whole module follows — but a *result* crosses a frame
    // boundary, and the storage it points at is the callee's own. So the one
    // place the value has to leave its slot is a `ret` of an aggregate, where
    // the target type is the struct itself: load it. Nothing else in this
    // emitter ever asks for a struct-typed target, since ParamTypeOfLayout turns
    // every aggregate *parameter* into a `ptr`.
    if ( Target->isStructTy() and Value->getType()->isPointerTy() )
    {
        return ServicesPtr->Ctx->Builder().CreateLoad( Target, Value );
    }
    return Value;
}
