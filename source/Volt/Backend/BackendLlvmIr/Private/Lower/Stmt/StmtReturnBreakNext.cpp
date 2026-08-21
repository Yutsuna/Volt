// StmtReturnBreakNext.cpp — the three exits.
//
// `return` is ordinary. `break` and `next` each mean two different things
// depending on whether this frame owns a loop: a branch inside one, and a
// non-local exit out of a closure body when there is none — which is the
// unwind-sentinel transport the exception lowering already owns, not a second
// mechanism.

#include "Lower/Stmt/StmtEmitter.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/Closure/ClosureLowering.hpp"
#include "Lower/Exception/ExceptionLowering.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include <llvm/IR/IRBuilder.h>

#include <string>

void Volt::Backend::Llvm::EmitReturn ( BodyEmitter &Emitter, const Frontend::Return &Node )
{
    FunctionFrame &Frame       = Emitter.Frame();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();

    if ( not Node.Value.IsValid() )
    {
        static_cast<void>( Builder.CreateRetVoid() );
        return;
    }

    llvm::Value *Value = Emitter.EmitExpr( Node.Value );
    if ( Value == nullptr )
    {
        return;
    }

    // A `return v` in a function the signature says returns nothing is a
    // middle-end disagreement, not something to silently drop: one of the two
    // read a different signature.
    if ( Frame.Fn->getReturnType()->isVoidTy() )
    {
        static_cast<void>( Emitter.Fail( "llvm: `return` with a value in '" + Frame.Fn->getName().str() +
                                         "', whose signature declares no result" ) );
        return;
    }
    static_cast<void>( Builder.CreateRet( Emitter.CoerceWidth( Value, Frame.Fn->getReturnType() ) ) );
}

void Volt::Backend::Llvm::EmitBreak ( BodyEmitter &Emitter, Frontend::StmtId Id, const Frontend::Break &Node )
{
    FunctionFrame &Frame       = Emitter.Frame();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();

    if ( Frame.Loops.empty() )
    {
        if ( not Frame.bClosure )
        {
            static_cast<void>( Emitter.Fail( "llvm: `break` outside a loop reached codegen" ) );
            return;
        }
        // Inside a closure with no loop of its own, `break` leaves the
        // *iterating* method — a non-local exit out of a frame this one does not
        // own. `break <value>` stays refused (backend phase 6d): there is no
        // result slot anywhere on this transport to carry it in, the same wall a
        // `while` hits below, just one call frame further out.
        if ( Node.Value.IsValid() )
        {
            static_cast<void>(
                Emitter.Fail( "llvm: `break` with a value at statement " + std::to_string( Id.Value ) +
                              " leaves a closure body — there is no result slot on the unwind transport to carry it in" ) );
            return;
        }
        static_cast<void>( Builder.CreateStore( Builder.getTrue(), Emitter.Services().Exceptions->BreakFlagSlot( Builder ) ) );
        Emitter.Services().Exceptions->EmitPoisonedPath( Emitter );
        return;
    }
    // `break v` yields a value out of a loop, and a `while` has no value to
    // yield it as.
    if ( Node.Value.IsValid() )
    {
        static_cast<void>( Emitter.Fail( "llvm: `break` with a value at statement " + std::to_string( Id.Value ) +
                                         " — a `while` has no value to yield it as" ) );
        return;
    }
    static_cast<void>( Builder.CreateBr( Frame.Loops.back().Merge ) );
}

void Volt::Backend::Llvm::EmitNext ( BodyEmitter &Emitter, Frontend::StmtId Id, const Frontend::Next &Node )
{
    FunctionFrame &Frame       = Emitter.Frame();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();

    // In a closure with no enclosing loop, `next` *is* the block's result: it
    // ends this invocation, which is a `ret`, not a branch. The same word means
    // "continue" only when there is a loop in this frame to continue.
    if ( Frame.Loops.empty() and Frame.bClosure )
    {
        Emitter.Services().Closures->EmitBlockNext( Emitter, Node.Value );
        return;
    }
    if ( Frame.Loops.empty() )
    {
        static_cast<void>( Emitter.Fail( "llvm: `next` outside a loop reached codegen" ) );
        return;
    }
    if ( Node.Value.IsValid() )
    {
        static_cast<void>( Emitter.Fail( "llvm: `next` with a value at statement " + std::to_string( Id.Value ) +
                                         " — a `while` iteration has no value to carry" ) );
        return;
    }
    static_cast<void>( Builder.CreateBr( Frame.Loops.back().Latch ) );
}
