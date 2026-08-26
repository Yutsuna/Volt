// PoisonedPath.cpp — the exit every raising path shares.
//
// Once this function decides it does not handle what is in flight: branch to the
// innermost `begin` it owns, or return early carrying a poisoned value. That
// early return *is* the propagation step — the caller's own post-call check is
// what continues the unwind one frame further.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

void Volt::Backend::Llvm::ExceptionLowering::EmitPoisonedPath ( BodyEmitter &Emitter )
{
    if ( Emitter.Terminated() )
    {
        return;
    }

    FunctionFrame &Frame       = Emitter.Frame();
    llvm::IRBuilder<> &Builder = Services->Ctx->Builder();

    // A `begin` owned by *this* function catches first; nothing further up the
    // AST does, since Tier 1 never crosses a call boundary except by this
    // early-return protocol — the caller's own post-call check is what continues
    // the unwind one frame further.
    if ( not Frame.Rescues.empty() )
    {
        static_cast<void>( Builder.CreateBr( Frame.Rescues.back() ) );
        return;
    }

    if ( Frame.bReturnsValue )
    {
        // Volt has no divergent/bottom type to make "this never really returns" a
        // typing question (rules/core-ast.md lists the closest relative: a
        // value-returning body falling off its end), so the poisoned value is
        // simply the type's zero — the caller's post-call check reads the
        // thread-local state, never this value.
        static_cast<void>( Builder.CreateRet( llvm::Constant::getNullValue( Frame.Fn->getReturnType() ) ) );
    }
    else
    {
        static_cast<void>( Builder.CreateRetVoid() );
    }
}
